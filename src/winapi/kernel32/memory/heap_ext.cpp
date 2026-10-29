/// @file heap_ext.cpp
/// @brief Extended heap operations implementation (WIN32-061)

#include "winapi/kernel32/memory/heap_ext.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/cpu/state/cpu_state.hpp"

#include <atomic>
#include <cstdio>
#include <utility>

namespace vitreo::winapi {

namespace {

/// ExceptionFlags observed for HeapAlloc/HeapReAlloc HEAP_GENERATE_EXCEPTIONS
/// OOM (heap_contract_probe children cases 0xa–0xe) — applied in arm_heap_oom_exception.
void raise_heap_no_memory(Marshaler& m, u64 request_size) {
    auto* cpu = m.cpu();
    if (cpu == nullptr) return;
    arm_heap_oom_exception(*cpu, request_size);
}

[[nodiscard]] bool heap_wants_exceptions(HeapManager& mgr, GuestAddr heap,
                                         u32 flags) noexcept {
    return (flags & HEAP_GENERATE_EXCEPTIONS) != 0 ||
           (mgr.heap_options(heap) & HEAP_GENERATE_EXCEPTIONS) != 0;
}

}  // namespace

void arm_heap_oom_exception(cpu::CpuState& cpu, u64 request_size) noexcept {
    cpu.pending_raise.active = true;
    cpu.pending_raise.from_helper = false;
    cpu.pending_raise.code = 0xC0000017u;  // STATUS_NO_MEMORY
    cpu.pending_raise.flags = 0x80u;       // heap_contract_probe ExceptionFlags
    cpu.pending_raise.num_args = 1;
    cpu.pending_raise.args[0] = request_size;
}

/// STATUS_HEAP_CORRUPTION for HeapFree/HeapSize/HeapReAlloc on a bad block
/// (heap_contract_probe children cases 0x1–0x3, 0x7–0x8: code=0xc0000374,
/// flags=0x81, nparams=1, p0 nonzero).
void arm_heap_corruption(cpu::CpuState& cpu) noexcept {
    cpu.pending_raise.active = true;
    cpu.pending_raise.from_helper = false;
    cpu.pending_raise.code = 0xC0000374u;
    cpu.pending_raise.flags = 0x81u;
    cpu.pending_raise.num_args = 1;
    cpu.pending_raise.args[0] = 1;
}

/// Invalid heap handle dereference (probe cases 0x4, 0xf): ACCESS_VIOLATION.
void arm_heap_access_violation(cpu::CpuState& cpu) noexcept {
    cpu.pending_raise.active = true;
    cpu.pending_raise.from_helper = false;
    cpu.pending_raise.code = 0xC0000005u;
    cpu.pending_raise.flags = 0;
    cpu.pending_raise.num_args = 2;
    cpu.pending_raise.args[0] = 0;
    cpu.pending_raise.args[1] = 1;
}

HeapExtended::HeapExtended(HeapManager& heap_mgr, HandleTable& handles)
    : m_heap_mgr(heap_mgr), m_handles(handles) {}

bool HeapExtended::heap_validate(GuestAddr heap, u32 /*flags*/, u64 address) {
    if (address == 0) {
        // Validate entire heap — always return true for our simple implementation
        return m_heap_mgr.heap_valid(heap);
    }
    // Validate specific allocation — check if HeapSize returns non-error
    u64 sz = m_heap_mgr.heap_size(heap, 0, address);
    return sz != ~0ULL;
}

u64 HeapExtended::heap_compact(GuestAddr /*heap*/, u32 /*flags*/) {
    // Return approximate largest free block — return a large constant
    return 0x100000; // 1 MB
}

u32 HeapExtended::get_process_heaps(u32 count, GuestAddr* heaps_out) {
    // Learn GetProcessHeaps: the return value is the number of live heaps;
    // min(count, that) handles are stored.
    const std::vector<GuestAddr> heaps = m_heap_mgr.heap_handles();
    for (u32 i = 0; heaps_out != nullptr && i < count && i < heaps.size(); ++i) {
        heaps_out[i] = heaps[i];
    }
    return static_cast<u32>(heaps.size());
}

DllExports create_heap_ext_thunks(HeapExtended& ext, HeapManager& mgr) {
    DllExports exports;

    // HeapReAlloc (4 args, stdcall_bytes=16)
    exports.entries.push_back({"HeapReAlloc", 1, [&mgr](Marshaler& m) {
        auto heap  = m.arg_ptr(0);
        auto flags = static_cast<u32>(m.arg(1));
        auto ptr   = m.arg(2);
        auto size  = m.arg(3);
        auto result = mgr.heap_realloc(heap, flags, ptr, size);
        if (result == 0) {
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            if (heap_wants_exceptions(mgr, heap, flags)) {
                raise_heap_no_memory(m, size);
            }
        }
        m.set_return(result);
    }, 16});
    // HeapSize (3 args, stdcall_bytes=12)
    exports.entries.push_back({"HeapSize", 2, [&mgr](Marshaler& m) {
        auto heap  = m.arg_ptr(0);
        auto flags = static_cast<u32>(m.arg(1));
        auto ptr   = m.arg(2);
        auto sz = mgr.heap_size(heap, flags, ptr);
        m.set_return(sz);
    }, 12});
    // HeapValidate (3 args, stdcall_bytes=12)
    exports.entries.push_back({"HeapValidate", 3, [&ext](Marshaler& m) {
        auto heap    = m.arg_ptr(0);
        auto flags   = static_cast<u32>(m.arg(1));
        auto address = m.arg(2);
        bool ok = ext.heap_validate(heap, flags, address);
        m.set_return(ok ? 1u : 0u);
    }, 12});
    // HeapCompact (2 args, stdcall_bytes=8)
    exports.entries.push_back({"HeapCompact", 4, [&ext](Marshaler& m) {
        auto heap  = m.arg_ptr(0);
        auto flags = static_cast<u32>(m.arg(1));
        auto sz = ext.heap_compact(heap, flags);
        m.set_return(sz);
    }, 8});
    // HeapCreate (3 args, stdcall_bytes=12)
    exports.entries.push_back({"HeapCreate", 5, [&mgr](Marshaler& m) {
        auto options     = static_cast<u32>(m.arg(0));
        auto initial_size = m.arg(1);
        auto max_size     = m.arg(2);
        auto h = mgr.heap_create(options, initial_size, max_size);
        m.set_return(static_cast<u64>(h));
    }, 12});
    // HeapDestroy (1 arg, stdcall_bytes=4)
    exports.entries.push_back({"HeapDestroy", 6, [&mgr](Marshaler& m) {
        auto heap = m.arg_ptr(0);
        // Windows: HeapDestroy(NULL) succeeds and leaves last-error untouched
        // (heap_contract_probe HeapDestroy_null ret=1 err=0xdead).
        if (heap == 0) {
            m.set_return(1);
            return;
        }
        bool ok = mgr.heap_destroy(heap);
        if (!ok) {
            // Learn: call GetLastError; Windows may AV on a stale pointer —
            // handle-table model reports ERROR_INVALID_HANDLE.
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            if (auto* cpu = m.cpu()) {
                arm_heap_access_violation(*cpu);
            }
        }
        m.set_return(ok ? 1u : 0u);
    }, 4});
    // GetProcessHeaps (2 args, stdcall_bytes=8)
    exports.entries.push_back({"GetProcessHeaps", 7, [&mgr](Marshaler& m) {
        auto count = static_cast<u32>(m.arg(0));
        auto out   = m.arg_ptr(1);
        // Learn GetProcessHeaps: returns the live heap count and stores
        // min(NumberOfHeaps, count) HANDLEs (pointer-sized) in the buffer.
        const std::vector<GuestAddr> heaps = mgr.heap_handles();
        const usize slot = m.cpu_is_64bit() ? 8u : 4u;
        for (u32 i = 0; out != 0 && i < count && i < heaps.size(); ++i) {
            (void)m.write_ptr(out + i * slot, heaps[i]);
        }
        m.set_return(static_cast<u64>(heaps.size()));
    }, 8});

    // HeapSetInformation (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "HeapSetInformation", 0,
        [](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER); m.set_return(0); return; }
        (void)m.arg(0); m.set_return(1); },
        16
    });

    // HeapAlloc — allocate from a heap (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "HeapAlloc", 0,
        [&mgr](Marshaler& m) {
            auto heap  = m.arg_ptr(0);
            auto flags = static_cast<u32>(m.arg(1));
            auto size  = m.arg(2);
            auto addr  = mgr.heap_alloc(heap, flags, size);
            if (addr == 0) {
                set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                if (heap_wants_exceptions(mgr, heap, flags)) {
                    raise_heap_no_memory(m, size);
                }
            }
            if (addr == 0 && size > (1ull << 40)) {
                if (m.cpu() != nullptr && m.cpu()->is_64bit()) {
                    const auto& cpu = *m.cpu();
                    const GuestAddr sp = cpu.sp();
                    std::fprintf(stderr,
                        "[heap] HeapAlloc huge-fail size=0x%llx rip=0x%llx "
                        "sp=0x%llx rbp=0x%llx rbx=0x%llx r12=0x%llx r13=0x%llx "
                        "r14=0x%llx r15=0x%llx rcx=0x%llx rdx=0x%llx r8=0x%llx\n",
                        static_cast<unsigned long long>(size),
                        static_cast<unsigned long long>(cpu.rip),
                        static_cast<unsigned long long>(sp),
                        static_cast<unsigned long long>(cpu.gpr.rbp),
                        static_cast<unsigned long long>(cpu.gpr.rbx),
                        static_cast<unsigned long long>(cpu.gpr.r12),
                        static_cast<unsigned long long>(cpu.gpr.r13),
                        static_cast<unsigned long long>(cpu.gpr.r14),
                        static_cast<unsigned long long>(cpu.gpr.r15),
                        static_cast<unsigned long long>(cpu.gpr.rcx),
                        static_cast<unsigned long long>(cpu.gpr.rdx),
                        static_cast<unsigned long long>(cpu.gpr.r8));
                    for (u32 i = 0; i < 40; ++i) {
                        const u64 slot = m.memory().read_u64(sp + i * 8).value_or(0);
                        std::fprintf(stderr, "  [%u]=0x%llx\n", i,
                            static_cast<unsigned long long>(slot));
                    }
                    // descriptor.cc CreateFlatAlloc frame: saved dest object
                    // lives at [rbp+0x20]; counts are at dest+0x70.
                    const GuestAddr dest =
                        m.memory().read_u64(cpu.gpr.rbp + 0x20).value_or(0);
                    std::fprintf(stderr,
                        "[heap] huge-fail dest=0x%llx rdi=0x%llx counts:",
                        static_cast<unsigned long long>(dest),
                        static_cast<unsigned long long>(cpu.gpr.rdi));
                    if (dest != 0) {
                        for (u32 i = 0; i < 16; ++i) {
                            const u32 count = m.memory()
                                .read_u32(dest + 0x70 + i * 4)
                                .value_or(0);
                            std::fprintf(stderr, " [%u]=0x%x", i,
                                static_cast<unsigned>(count));
                        }
                        std::fprintf(stderr, "\n[heap] huge-fail dest qwords:");
                        for (u32 i = 0; i < 28; ++i) {
                            const u64 q = m.memory()
                                .read_u64(dest + i * 8)
                                .value_or(0);
                            std::fprintf(stderr, " [+0x%02x]=0x%llx", i * 8,
                                static_cast<unsigned long long>(q));
                        }
                    }
                    std::fprintf(stderr, "\n");
                }
            }
            m.set_return(addr);
        },
        12
    });

    // HeapFree — free heap memory (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "HeapFree", 0,
        [&mgr](Marshaler& m) {
            auto heap    = m.arg_ptr(0);
            auto flags   = static_cast<u32>(m.arg(1));
            auto address = m.arg(2);
            // The guest return address makes a premature/duplicate free
            // attributable to the module that issued it.
            const auto caller = m.cpu()
                ? m.read_u32(static_cast<GuestAddr>(m.cpu()->sp())).value_or(0)
                : 0;
            // A NULL pointer is explicitly legal and succeeds -- free(NULL)
            // in the CRT lands here, so returning FALSE makes ordinary
            // cleanup code look like it is double-freeing.
            if (address == 0) {
                m.set_return(1);
                return;
            }
            bool ok = mgr.heap_free(heap, flags, address);
            if (!ok) {
                // Windows terminates with STATUS_HEAP_CORRUPTION (valid heap,
                // bad block) or STATUS_ACCESS_VIOLATION (invalid heap handle).
                // Soft FALSE alone leaves GetLastError stale
                // (heap_contract_probe children 0x1–0x4, 0xf).
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                if (auto* cpu = m.cpu()) {
                    if (mgr.heap_valid(heap)) {
                        arm_heap_corruption(*cpu);
                    } else {
                        arm_heap_access_violation(*cpu);
                    }
                }
            }
            VITREO_TRACE("[heap] HeapFree(0x%llx) caller=0x%08x -> %d\n",
                         static_cast<unsigned long long>(address), caller,
                         ok ? 1 : 0);
            m.set_return(ok ? 1u : 0u);
        },
        12
    });

    // GetProcessHeap — return process heap handle (0 args, stdcall_bytes=0)
    exports.entries.push_back(ExportEntry{
        "GetProcessHeap", 0,
        [&mgr](Marshaler& m) {
            auto h = mgr.get_process_heap();
            m.set_return(static_cast<u64>(h));
        },
        0
    });

    return exports;
}

// =============================================================================
// ntdll Rtl heap
// =============================================================================
//
// Learn (RtlCreateHeap, RtlDestroyHeap, RtlAllocateHeap, RtlFreeHeap)
// documents the parameters; tools/oracle_probe/rtl_heap_probe (x64 and x86)
// measures the rest: a heap from either API works with both, every heap is
// listed by GetProcessHeaps until destroyed, RtlCreateHeap ignores
// Parameters whatever their Length, refuses a Lock with HEAP_NO_SERIALIZE,
// and creates a non-growable heap without HEAP_GROWABLE (Learn's "must be
// specified" notwithstanding) that refuses a block past its reserve;
// RtlDestroyHeap returns NULL on success and refuses the process heap by
// returning it; RtlFreeHeap(NULL) is TRUE.

DllExports create_rtl_heap_thunks(HeapExtended& ext, HeapManager& mgr) {
    DllExports exports;

    // kernel32's HeapAlloc / HeapReAlloc / HeapSize are forwarders to these
    // (dumpbin /exports kernel32.dll: "forwarded to NTDLL.RtlAllocateHeap"),
    // so they are one implementation under two names.
    const DllExports k32 = create_heap_ext_thunks(ext, mgr);
    const std::pair<const char*, const char*> forwarded[] = {
        {"HeapAlloc", "RtlAllocateHeap"},
        {"HeapReAlloc", "RtlReAllocateHeap"},
        {"HeapSize", "RtlSizeHeap"},
    };
    for (const auto& [kernel32_name, ntdll_name] : forwarded) {
        for (const auto& entry : k32.entries) {
            if (entry.name == kernel32_name) {
                exports.entries.push_back({ntdll_name, 0, entry.handler, entry.stdcall_bytes});
            }
        }
    }

    // BOOLEAN RtlFreeHeap(PVOID HeapHandle, ULONG Flags, PVOID BaseAddress).
    // HeapFree is KernelBase's wrapper that adds the last error; RtlFreeHeap
    // raises the same failures without touching it.
    const auto rtl_free = [&mgr](Marshaler& m, GuestAddr heap, u32 flags, u64 address) {
        const bool ok = mgr.heap_free(heap, flags, address);
        if (!ok) {
            if (auto* cpu = m.cpu()) {
                if (mgr.heap_valid(heap)) {
                    arm_heap_corruption(*cpu);
                } else {
                    arm_heap_access_violation(*cpu);
                }
            }
        }
        return ok;
    };
    exports.entries.push_back({"RtlFreeHeap", 0, [rtl_free](Marshaler& m) {
        const auto heap = m.arg_ptr(0);
        const auto flags = static_cast<u32>(m.arg(1));
        const u64 address = m.arg(2);
        if (address == 0) {
            m.set_return(1);
            return;
        }
        m.set_return(rtl_free(m, heap, flags, address) ? 1u : 0u);
    }, 12});

    // VOID RtlFreeUnicodeString(PUNICODE_STRING UnicodeString). Learn: frees
    // a Buffer the Rtl string routines allocated, which is a process-heap
    // block (rtl_string_status_probe: a RtlAllocateHeap or HeapAlloc block
    // on the process heap no longer validates afterwards). With a Buffer the
    // whole structure is zeroed -- on x64 the padding after MaximumLength as
    // well (16 bytes: USHORT, USHORT, pad, PWSTR at +8; x86: 8 bytes, PWSTR
    // at +4); with a NULL Buffer it is left untouched.
    exports.entries.push_back({"RtlFreeUnicodeString", 0, [&mgr, rtl_free](Marshaler& m) {
        const GuestAddr string = m.arg_ptr(0);
        const bool is64 = m.cpu_is_64bit();
        const GuestAddr buffer_field = string + (is64 ? 8u : 4u);
        const auto buffer = m.read_ptr(buffer_field);
        if (!buffer.is_ok()) {
            (void)m.raise_access_violation(false, buffer_field);
            return;
        }
        if (buffer.value() == 0) return;
        if (!rtl_free(m, mgr.get_process_heap(), 0, buffer.value())) return;
        (void)m.write_u64(string, 0);
        if (is64) (void)m.write_u64(string + 8, 0);
    }, 4});

    // PVOID RtlCreateHeap(ULONG Flags, PVOID HeapBase, SIZE_T ReserveSize,
    //                     SIZE_T CommitSize, PVOID Lock,
    //                     PRTL_HEAP_PARAMETERS Parameters)
    exports.entries.push_back({"RtlCreateHeap", 0, [&mgr](Marshaler& m) {
        const u32 flags = static_cast<u32>(m.arg(0));
        const u64 heap_base = m.arg_ptr(1);
        const u64 reserve_arg = m.cpu_is_64bit() ? m.arg(2) : m.arg32(2);
        const u64 commit_arg = m.cpu_is_64bit() ? m.arg(3) : m.arg32(3);
        const u64 lock = m.arg_ptr(4);
        if (heap_base != 0) {
            // Windows builds the heap inside caller memory and returns
            // HeapBase itself (rtl_heap_probe caller_base); Vitreo heaps are
            // handle-table objects, not structures at an address.
            static std::atomic<bool> reported{false};
            if (!reported.exchange(true)) {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED ntdll!RtlCreateHeap: "
                                     "caller-supplied HeapBase -> NULL\n");
            }
            m.set_return(0);
            return;
        }
        // Learn: "If the HEAP_NO_SERIALIZE flag is set, this parameter must
        // be NULL" (rtl_heap_probe lock_no_base_noserialize: NULL). Without
        // it Windows serializes on the caller's lock; Vitreo's heap
        // serializes internally, which is the same exclusion.
        if (lock != 0 && (flags & HEAP_NO_SERIALIZE) != 0) {
            m.set_return(0);
            return;
        }
        // Learn's ReserveSize / CommitSize table: both zero reserves 64
        // pages; a zero reserve takes the commit rounded up to 16 pages; a
        // commit above the reserve is reduced to it. A non-growable heap's
        // maximum size is its reserve.
        constexpr u64 kPage = 0x1000;
        const auto round_up = [](u64 v, u64 a) { return (v + a - 1) & ~(a - 1); };
        u64 reserve = round_up(reserve_arg, kPage);
        u64 commit = round_up(commit_arg, kPage);
        if (reserve == 0) reserve = commit == 0 ? 64 * kPage : round_up(commit, 16 * kPage);
        if (commit == 0) commit = kPage;
        if (commit > reserve) commit = reserve;
        const bool growable = (flags & HEAP_GROWABLE) != 0;
        const GuestAddr heap = mgr.heap_create(flags, commit, growable ? 0 : reserve, true);
        m.set_return(heap == INVALID_HANDLE ? 0 : static_cast<u64>(heap));
    }, 24});

    // PVOID RtlDestroyHeap(PVOID HeapHandle): NULL on success, else the
    // handle. The process heap is refused (rtl_heap_probe
    // destroy_process_heap returns it).
    exports.entries.push_back({"RtlDestroyHeap", 0, [&mgr](Marshaler& m) {
        const u64 heap = m.arg_ptr(0);
        const bool destroyed = heap != 0 &&
                               heap != mgr.get_process_heap() &&
                               mgr.heap_destroy(heap);
        m.set_return(destroyed ? 0 : heap);
    }, 4});

    return exports;
}

} // namespace vitreo::winapi
