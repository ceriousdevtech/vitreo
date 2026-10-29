#include "app/cli/commands/guest_thread_context.hpp"
#include "winapi/kernel32/thread/thread_context.hpp"

#include "runtime/process/runtime_context.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/kernel32/thread/tls.hpp"

#include <vector>

namespace vitreo::cli {
namespace {

/// x64 guest threads get a larger reserve: the Windows x64 ABI's shadow space
/// plus deeper native frames make 1 MB tight for real applications.
constexpr u64 THREAD_STACK_SIZE_64 = 4ULL * 1024ULL * 1024ULL;
constexpr u64 THREAD_STACK_SIZE_32 = 1024ULL * 1024ULL;
constexpr u64 TEB_SIZE = 0x2000;

constexpr u32 ALLOC_FLAGS = winapi::MEM_COMMIT | winapi::MEM_RESERVE;

/// Copy one DLL's TLS template into a fresh per-thread block and publish it in
/// this thread's TLS array. Shared by both bitnesses; `slot_stride` and the
/// pointer width are all that differ.
void replay_dll_tls_slots(GuestMemory& memory, RuntimeContext& ctx,
                          u64 tls_array_addr, u32 capacity, u32 bitness,
                          u32 tid, std::vector<u64>* blocks_out) {
    const bool is_64bit = bitness == 64;
    for (const auto& slot : ctx.module_mgr.dll_tls_slots()) {
        if (slot.bitness != bitness) continue;
        // A DLL loaded after this array was sized: register_thread_teb grows
        // the array and fills the slot under the loader's TLS lock.
        if (slot.slot_index >= capacity) continue;
        u64 block_size = slot.template_size + slot.zero_fill;
        if (block_size == 0) block_size = 256;
        // Whole host pages: see round_up_host_page.
        const u64 block_alloc = round_up_host_page(block_size);
        const u64 block_addr =
            ctx.vmm.virtual_alloc(0, block_alloc, ALLOC_FLAGS,
                                  winapi::PAGE_READWRITE);
        if (block_addr == 0) continue;
        if (blocks_out != nullptr) blocks_out->push_back(block_addr);

        if (slot.template_size > 0) {
            std::vector<u8> tmpl(static_cast<usize>(slot.template_size));
            auto read = memory.read_bytes(slot.template_start, tmpl.data(),
                                          static_cast<usize>(slot.template_size));
            if (read.is_ok()) {
                (void)memory.write_bytes(block_addr, tmpl.data(),
                                         static_cast<usize>(slot.template_size));
            }
        }

        if (is_64bit) {
            (void)memory.write_u64(tls_array_addr + slot.slot_index * 8u,
                                   block_addr);
        } else {
            (void)memory.write_u32(tls_array_addr + slot.slot_index * 4u,
                                   static_cast<u32>(block_addr));
        }
        VITREO_TRACE(
            "[thread] DLL TLS tid=%u slot=%u data=0x%llx (%llu bytes) \"%s\"\n",
            tid, slot.slot_index,
            static_cast<unsigned long long>(block_addr),
            static_cast<unsigned long long>(slot.template_size),
            slot.dbg_path.c_str());
    }
}

void setup_teb_64(GuestMemory& memory, RuntimeContext& ctx,
                  const GuestThreadContextParams& params, u32 tid,
                  u64 teb_addr, u64 stack_top, u64 stack_bottom,
                  GuestThreadContext& out) {
    (void)memory.write_u64(teb_addr + 0x00, 0);                  // ExceptionList
    (void)memory.write_u64(teb_addr + 0x08, stack_top);          // StackBase
    (void)memory.write_u64(teb_addr + 0x10, stack_bottom);       // StackLimit
    // DeallocationStack: the base of the whole stack allocation, which
    // GetCurrentThreadStackLimits reports as LowLimit and which marks the
    // bottom of the region an overflow is recognised in. Left at zero it
    // read as "the stack extends to address 0".
    (void)memory.write_u64(teb_addr + 0x1478, out.stack_base);
    (void)memory.write_u64(teb_addr + 0x30, teb_addr);           // Self
    (void)memory.write_u64(teb_addr + 0x40, ctx.process_mgr.get_current_process_id());                  // ClientId.UniqueProcess
    (void)memory.write_u64(teb_addr + 0x48, static_cast<u64>(tid)); // ClientId.UniqueThread
    (void)memory.write_u64(teb_addr + 0x60, params.peb_address); // PEB
    (void)memory.write_u32(teb_addr + 0x68, 0);                  // LastErrorValue

    // Always allocate a per-thread TLS array (TEB+0x58) so DLL
    // __declspec(thread) storage works on worker threads, even when the main
    // module itself has no TLS directory.
    const u64 main_tls_bytes =
        (params.pe_tls_data_size > 0)
            ? ((static_cast<u64>(params.pe_tls_data_size) + 0xFFFu) & ~0xFFFULL)
            : 0;
    // The array covers every implicit-TLS index handed out so far (it can
    // exceed one page in a process with hundreds of TLS-bearing DLLs).
    const u64 array_bytes =
        (static_cast<u64>(ctx.module_mgr.tls_array_capacity()) * 8u + 0xFFFu) &
        ~0xFFFULL;
    const u64 tls_alloc_size = array_bytes + main_tls_bytes;
    const u64 tls_base = ctx.vmm.virtual_alloc(0, round_up_host_page(tls_alloc_size), ALLOC_FLAGS,
                                               winapi::PAGE_READWRITE);
    if (tls_base == 0) return;
    out.tls_base = tls_base;
    out.tls_capacity = static_cast<u32>(array_bytes / 8u);

    (void)memory.write_u64(teb_addr + 0x58, tls_base);  // ThreadLocalStoragePointer

    // Main module TLS -> slot 0 (if the exe has a TLS directory).
    if (params.pe_tls_data_size > 0) {
        const u64 tls_data_alloc = tls_base + array_bytes;
        std::vector<u8> tmpl(params.pe_tls_data_size);
        auto read = memory.read_bytes(params.pe_tls_data_start, tmpl.data(),
                                      params.pe_tls_data_size);
        if (read.is_ok()) {
            (void)memory.write_bytes(tls_data_alloc, tmpl.data(),
                                     params.pe_tls_data_size);
        }
        (void)memory.write_u64(tls_base, tls_data_alloc);
    }

    // Replay every x64 DLL TLS slot so DLL __declspec(thread) accessors read
    // a valid per-thread block instead of a null TLS array entry, which
    // faults as
    // `mov rax,gs:[0x58]; mov rax,[rax+idx*8]; mov eax,[rax+off]`.
    // Without this, x64 worker threads deadlock or relaunch.
    replay_dll_tls_slots(memory, ctx, tls_base, out.tls_capacity, 64, tid,
                         &out.dll_tls_blocks);
}

void setup_teb_32(GuestMemory& memory, RuntimeContext& ctx,
                  const GuestThreadContextParams& params, u32 tid,
                  u64 teb_addr, u64 stack_top, u64 stack_bottom,
                  GuestThreadContext& out) {
    (void)memory.write_u32(teb_addr + 0x00, 0xFFFFFFFF);                    // ExceptionList (none)
    (void)memory.write_u32(teb_addr + 0x04, static_cast<u32>(stack_top));    // StackBase
    (void)memory.write_u32(teb_addr + 0x08, static_cast<u32>(stack_bottom)); // StackLimit
    (void)memory.write_u32(teb_addr + 0x18, static_cast<u32>(teb_addr));     // Self
    (void)memory.write_u32(teb_addr + 0x20, ctx.process_mgr.get_current_process_id());                             // ClientId.UniqueProcess
    (void)memory.write_u32(teb_addr + 0x24, tid);                           // ClientId.UniqueThread
    (void)memory.write_u32(teb_addr + 0x30,
                           static_cast<u32>(params.peb_address));           // PEB
    (void)memory.write_u32(teb_addr + 0x34, 0);                            // LastErrorValue

    // Always allocate TEB.ThreadLocalStoragePointer (fs:[0x2c]), matching the
    // x64 path and Windows: DLL __declspec(thread) storage, and guest code
    // that indexes the PE TLS array, require a per-thread array even when
    // the main EXE has no TLS directory.
    const u32 main_tls_bytes =
        (params.pe_tls_data_size > 0)
            ? ((params.pe_tls_data_size + 0xFFFu) & ~0xFFFu)
            : 0;
    // The array covers every implicit-TLS index handed out so far; the EXE's
    // own block follows it on the next page boundary.
    const u32 array_bytes =
        (ctx.module_mgr.tls_array_capacity() * 4u + 0xFFFu) & ~0xFFFu;
    const u32 tls_alloc_size = array_bytes + main_tls_bytes;
    const u64 tls_base = ctx.vmm.virtual_alloc(0, round_up_host_page(tls_alloc_size), ALLOC_FLAGS,
                                               winapi::PAGE_READWRITE);
    if (tls_base == 0) return;
    out.tls_base = tls_base;
    out.tls_capacity = array_bytes / 4u;

    const u32 tls_array_addr = static_cast<u32>(tls_base);
    (void)memory.write_u32(static_cast<u32>(teb_addr) + 0x2C,
                           tls_array_addr);  // TEB.TlsPointer

    if (params.pe_tls_data_size > 0) {
        const u32 tls_data_alloc = tls_array_addr + array_bytes;
        std::vector<u8> tmpl(params.pe_tls_data_size);
        auto read = memory.read_bytes(params.pe_tls_data_start, tmpl.data(),
                                      params.pe_tls_data_size);
        if (read.is_ok()) {
            (void)memory.write_bytes(tls_data_alloc, tmpl.data(),
                                     params.pe_tls_data_size);
        }
        (void)memory.write_u32(tls_array_addr, tls_data_alloc);  // array[0]
        VITREO_TRACE(
            "[thread] TLS for tid=%u: array=0x%x data=0x%x (%u bytes)\n", tid,
            tls_array_addr, tls_data_alloc, params.pe_tls_data_size);
    } else {
        VITREO_TRACE("[thread] TLS array for tid=%u: array=0x%x (no EXE TLS)\n",
                     tid, tls_array_addr);
    }

    // setup_dll_tls only seeded the main thread; new threads need the same
    // layout or they fault inside __declspec(thread).
    replay_dll_tls_slots(memory, ctx, tls_array_addr, out.tls_capacity, 32,
                         tid, &out.dll_tls_blocks);
}

} // namespace

GuestThreadContext create_guest_thread_context(
    GuestMemory& memory, RuntimeContext& ctx,
    const GuestThreadContextParams& params, u32 tid) {
    GuestThreadContext context;
    context.is_64bit = params.is_64bit;
    // A guest thread gets exactly the reservation CreateThread computed from
    // dwStackSize and the EXE's header (the whole reservation is committed;
    // Vitreo does not grow stacks through a guard page). Vitreo's own service
    // threads pass 0 and keep the fixed size.
    context.stack_size =
        params.stack_reserve != 0
            ? params.stack_reserve
            : (params.is_64bit ? THREAD_STACK_SIZE_64 : THREAD_STACK_SIZE_32);

    context.stack_base = ctx.vmm.virtual_alloc(0, context.stack_size,
                                               ALLOC_FLAGS,
                                               winapi::PAGE_READWRITE);
    if (context.stack_base == 0) {
        VITREO_TRACE("[thread] ERROR: Failed to allocate stack for tid=%u\n", tid);
        return {};
    }

    // The TEB's region size is the runtime's choice (Windows places TEBs
    // itself); whole host pages keep every gs:-relative access on the
    // hardware-protected direct path. See round_up_host_page.
    context.teb_addr = ctx.vmm.virtual_alloc(0, round_up_host_page(TEB_SIZE),
                                             ALLOC_FLAGS,
                                             winapi::PAGE_READWRITE);
    if (context.teb_addr == 0) {
        VITREO_TRACE("[thread] ERROR: Failed to allocate TEB for tid=%u\n", tid);
        return {};
    }

    const u64 stack_top = context.stack_base + context.stack_size;
    u64 stack_bottom = context.stack_base;

    // Stack overflow guard, x64. Measured on Windows
    // (tools/oracle_probe/stack_overflow_probe.c): a thread that runs off its
    // stack gets STATUS_STACK_OVERFLOW when a write lands within 16 KiB of the
    // allocation base, and its handlers then run on the pages between there
    // and base + 4 KiB, which is where the kernel leaves StackLimit.
    //
    // Without a guard the stack was flat to the allocation base, so an
    // overflow ran into unmapped memory with nothing left to dispatch on: the
    // exception frame's own writes faulted, no handler ran, and the thread
    // died without a word. The guard is what makes the fault arrive while
    // there is still room; exception dispatch recognises it by the TEB
    // bounds and opens the recovery pages.
    //
    // 32-bit threads keep the flat layout: only the x64 dispatcher performs
    // the conversion, and a guard it does not recognise would only move the
    // same silent fault 16 KiB higher.
    if (params.is_64bit) {
        constexpr u64 kStackOverflowGuardBytes = 0x4000;
        u32 old_protect = 0;
        if (ctx.vmm.virtual_protect(context.stack_base,
                                    kStackOverflowGuardBytes,
                                    winapi::PAGE_NOACCESS, &old_protect)) {
            stack_bottom = context.stack_base + kStackOverflowGuardBytes;
        }
    }

    if (params.is_64bit) {
        setup_teb_64(memory, ctx, params, tid, context.teb_addr, stack_top,
                     stack_bottom, context);
    } else {
        setup_teb_32(memory, ctx, params, tid, context.teb_addr, stack_top,
                     stack_bottom, context);
    }
    ctx.module_mgr.register_thread_teb(
        context.teb_addr, params.is_64bit ? 64u : 32u, context.tls_capacity);
    ctx.tls_mgr.register_teb(memory, context.teb_addr, params.is_64bit);

    VITREO_TRACE("[thread] TEB at 0x%llx for tid=%u (stack 0x%llx-0x%llx)\n",
                 static_cast<unsigned long long>(context.teb_addr), tid,
                 static_cast<unsigned long long>(stack_bottom),
                 static_cast<unsigned long long>(stack_top));
    return context;
}

void release_guest_thread_context(GuestMemory& memory, RuntimeContext& ctx,
                                  const GuestThreadContext& context) {
    // Unregister the TEB first: once its pages are released, a DLL that loads
    // afterwards must not replay a static-TLS slot into them.
    if (context.teb_addr != 0) {
        ctx.module_mgr.unregister_thread_teb(context.teb_addr);
        ctx.tls_mgr.unregister_teb(context.teb_addr);
        // Windows frees TEB.TlsExpansionSlots with the thread.
        winapi::release_tls_expansion_slots(memory, context.teb_addr,
                                            context.is_64bit);
    }
    // MEM_RELEASE takes size 0 and frees the whole reservation, matching how
    // each of these was allocated (one virtual_alloc per address).
    for (const u64 block : context.dll_tls_blocks) {
        (void)ctx.vmm.virtual_free(block, 0, winapi::MEM_RELEASE);
    }
    if (context.tls_base != 0) {
        (void)ctx.vmm.virtual_free(context.tls_base, 0, winapi::MEM_RELEASE);
    }
    if (context.teb_addr != 0) {
        (void)ctx.vmm.virtual_free(context.teb_addr, 0, winapi::MEM_RELEASE);
    }
    if (context.stack_base != 0) {
        (void)ctx.vmm.virtual_free(context.stack_base, 0, winapi::MEM_RELEASE);
    }
}

cpu::CpuState make_guest_cpu_state(GuestMemory& memory,
                                   const GuestThreadContext& context,
                                   bool is_64bit) {
    cpu::CpuState state{};
    if (is_64bit) {
        state.init_long_mode();
        winapi::seed_windows_initial_fpu(state);
        state.seg.gs_desc.base = context.teb_addr;
        state.set_sp(context.stack_base + context.stack_size);
        return state;
    }

    state.init_protected_mode();
    winapi::seed_windows_initial_fpu(state);
    state.seg.fs_desc.base = context.teb_addr;

    // Leave one page of headroom on 32-bit threads. Thread startup aligns a
    // pointer upward from the current frame and expects that page to be
    // committed.
    u64 stack_top = context.stack_base + context.stack_size - 0x1004;
    stack_top &= ~0x3ULL;
    (void)memory.write_u32(stack_top, 0);
    state.set_sp(stack_top);
    return state;
}

} // namespace vitreo::cli
