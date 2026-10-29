// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#include "runtime/diagnostics/host_resource_probe.hpp"

#include "runtime/diagnostics/hang_dump.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <dlfcn.h>
#include <execinfo.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_port.h>
#include <mach/mach_vm.h>
#include <mach/task_info.h>

// libmalloc's logging hook. Setting it is the same mechanism
// `MallocStackLogging` uses, but without the interposed logging library that
// does not survive in this process. The signature is fixed by libmalloc.
extern "C" void (*malloc_logger)(uint32_t type, uintptr_t arg1, uintptr_t arg2,
                                 uintptr_t arg3, uintptr_t result,
                                 uint32_t num_hot_frames_to_skip);

namespace vitreo::diagnostics {

namespace {

// ---------------------------------------------------------------------------
// Mach port census
// ---------------------------------------------------------------------------

/// Kernel object classes behind a port right, as reported by
/// `mach_port_kobject`. The ordering is XNU's `ipc_kobject.h` IKOT_* list;
/// verified on this host by creating one of each and re-reading the census,
/// which is worth redoing after a major macOS update since the list only ever
/// grows in the middle.
const char* kobject_name(natural_t type) {
    switch (type) {
        case 0:  return "none";
        case 1:  return "thread-control";
        case 2:  return "task-control";
        case 3:  return "host";
        case 4:  return "host-priv";
        case 5:  return "processor";
        case 6:  return "pset";
        case 7:  return "pset-name";
        case 8:  return "timer";
        case 9:  return "port-subst-once";
        case 10: return "mig";
        case 11: return "memory-object";
        case 12: return "xmm-pager";
        case 13: return "xmm-kernel";
        case 14: return "xmm-reply";
        case 15: return "und-reply";
        case 16: return "host-notify";
        case 17: return "host-security";
        case 18: return "ledger";
        case 19: return "main-device";
        case 20: return "task-name";
        case 21: return "subsystem";
        case 22: return "io-done-queue";
        case 23: return "semaphore";
        case 24: return "lock-set";
        case 25: return "clock";
        case 26: return "clock-ctrl";
        case 27: return "iokit-ident";
        case 28: return "named-entry";
        case 29: return "iokit-connect";
        case 30: return "iokit-object";
        case 31: return "upl";
        case 32: return "mem-obj-control";
        case 33: return "au-sessionport";
        case 34: return "fileport";
        case 35: return "labelh";
        case 36: return "task-resume";
        case 37: return "voucher";
        case 38: return "voucher-attr-control";
        case 39: return "work-interval";
        case 40: return "ux-handler";
        case 41: return "uext-object";
        case 42: return "arcade-reg";
        case 43: return "eventlink";
        case 44: return "task-inspect";
        case 45: return "task-read";
        case 46: return "thread-inspect";
        case 47: return "thread-read";
        case 48: return "setuid-cred";
        case 49: return "task-id-token";
        case 50: return "task-fatal";
        case 51: return "kcdata";
        case 52: return "exclaves-resource";
        default: return "other";
    }
}

struct KobjectBucket {
    natural_t type = 0;
    unsigned long count = 0;
};

void dump_mach_ports(std::FILE* out) {
    mach_port_name_array_t names = nullptr;
    mach_msg_type_number_t name_count = 0;
    mach_port_type_array_t types = nullptr;
    mach_msg_type_number_t type_count = 0;

    const kern_return_t kr = mach_port_names(mach_task_self(), &names,
                                             &name_count, &types, &type_count);
    if (kr != KERN_SUCCESS) {
        std::fprintf(out, "[host-res] pid=%d mach_port_names failed kr=%d\n",
                     getpid(), kr);
        return;
    }

    unsigned long send = 0, recv = 0, send_once = 0, dead = 0, port_set = 0;
    // Small fixed histogram: no allocation, and the kobject space is tiny.
    KobjectBucket buckets[48];
    unsigned bucket_used = 0;

    for (mach_msg_type_number_t i = 0; i < name_count; ++i) {
        const mach_port_type_t t = (i < type_count) ? types[i] : 0;
        if (t & MACH_PORT_TYPE_SEND) ++send;
        if (t & MACH_PORT_TYPE_RECEIVE) ++recv;
        if (t & MACH_PORT_TYPE_SEND_ONCE) ++send_once;
        if (t & MACH_PORT_TYPE_DEAD_NAME) ++dead;
        if (t & MACH_PORT_TYPE_PORT_SET) ++port_set;

        ipc_info_object_type_t okind = static_cast<ipc_info_object_type_t>(0);
        mach_vm_address_t oaddr = 0;
        if (mach_port_kobject(mach_task_self(), names[i], &okind, &oaddr)
            != KERN_SUCCESS) {
            okind = static_cast<ipc_info_object_type_t>(-1);
        }
        unsigned b = 0;
        for (; b < bucket_used; ++b) {
            if (buckets[b].type == static_cast<natural_t>(okind)) break;
        }
        if (b == bucket_used && bucket_used < 48) {
            buckets[bucket_used].type = static_cast<natural_t>(okind);
            buckets[bucket_used].count = 0;
            ++bucket_used;
        }
        if (b < 48) ++buckets[b].count;
    }

    std::fprintf(out,
                 "[host-res] pid=%d mach_ports total=%u send=%lu recv=%lu "
                 "send_once=%lu dead=%lu port_set=%lu\n",
                 getpid(), static_cast<unsigned>(name_count), send, recv,
                 send_once, dead, port_set);
    for (unsigned b = 0; b < bucket_used; ++b) {
        const char* name = buckets[b].type == static_cast<natural_t>(-1)
            ? "unknown"
            : kobject_name(buckets[b].type);
        std::fprintf(out, "[host-res] pid=%d   kobject %s(%d) = %lu\n",
                     getpid(), name, static_cast<int>(buckets[b].type),
                     buckets[b].count);
    }

    if (names != nullptr) {
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(names),
                      name_count * sizeof(*names));
    }
    if (types != nullptr) {
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(types),
                      type_count * sizeof(*types));
    }
}

// ---------------------------------------------------------------------------
// Large host allocation recorder
// ---------------------------------------------------------------------------

constexpr uint32_t kMallocLogTypeAllocate = 2;
constexpr uint32_t kMallocLogTypeDeallocate = 4;
constexpr uint32_t kMallocLogTypeHasZone = 8;

constexpr unsigned kMaxLiveRecords = 256;
constexpr unsigned kMaxFrames = 12;

struct LargeAllocRecord {
    std::atomic<uintptr_t> ptr{0};
    std::size_t size = 0;
    unsigned frame_count = 0;
    void* frames[kMaxFrames] = {};
};

LargeAllocRecord g_records[kMaxLiveRecords];
std::atomic<std::size_t> g_threshold{32u * 1024u * 1024u};
std::atomic<unsigned long> g_recorded{0};
std::atomic<unsigned long> g_overflowed{0};

void record_alloc(uintptr_t ptr, std::size_t size) {
    for (unsigned i = 0; i < kMaxLiveRecords; ++i) {
        uintptr_t expected = 0;
        if (!g_records[i].ptr.compare_exchange_strong(
                expected, ptr, std::memory_order_acq_rel)) {
            continue;
        }
        g_records[i].size = size;
        // backtrace() walks frame pointers and does not allocate.
        const int n = backtrace(g_records[i].frames, kMaxFrames);
        g_records[i].frame_count = n > 0 ? static_cast<unsigned>(n) : 0;
        g_recorded.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_overflowed.fetch_add(1, std::memory_order_relaxed);
}

void forget_alloc(uintptr_t ptr) {
    for (unsigned i = 0; i < kMaxLiveRecords; ++i) {
        if (g_records[i].ptr.load(std::memory_order_acquire) != ptr) continue;
        g_records[i].size = 0;
        g_records[i].frame_count = 0;
        g_records[i].ptr.store(0, std::memory_order_release);
        return;
    }
}

void large_alloc_logger(uint32_t type, uintptr_t arg1, uintptr_t arg2,
                        uintptr_t arg3, uintptr_t result, uint32_t) {
    // With MALLOC_LOG_TYPE_HAS_ZONE the zone is arg1 and the size is arg2;
    // realloc additionally reports the old pointer, and free reports the
    // pointer being released in the first non-zone argument.
    const bool has_zone = (type & kMallocLogTypeHasZone) != 0;
    const bool is_alloc = (type & kMallocLogTypeAllocate) != 0;
    const bool is_free = (type & kMallocLogTypeDeallocate) != 0;

    // No thread-local re-entrancy guard: touching a thread_local from inside
    // the malloc hook makes dyld instantiate the TLV block, which itself
    // mallocs, which re-enters the hook and recurses until the stack dies.
    // Nothing below allocates -- backtrace() only walks frame pointers.
    if (is_free) {
        const uintptr_t freed = has_zone ? arg2 : arg1;
        if (freed != 0) forget_alloc(freed);
    }
    if (is_alloc && result != 0) {
        std::size_t size = static_cast<std::size_t>(has_zone ? arg2 : arg1);
        if (is_free) {
            // realloc: size is the argument after the old pointer.
            size = static_cast<std::size_t>(has_zone ? arg3 : arg2);
        }
        if (size >= g_threshold.load(std::memory_order_relaxed)) {
            record_alloc(result, size);
        }
    }
}

void dump_large_allocs(std::FILE* out) {
    if (malloc_logger != &large_alloc_logger) {
        std::fprintf(out,
                     "[host-res] pid=%d large-alloc probe disabled "
                     "(set VITREO_LARGE_ALLOC_PROBE=1)\n",
                     getpid());
        return;
    }
    std::fprintf(out,
                 "[host-res] pid=%d large-alloc threshold=%zu recorded=%lu "
                 "overflowed=%lu\n",
                 getpid(), g_threshold.load(std::memory_order_relaxed),
                 g_recorded.load(std::memory_order_relaxed),
                 g_overflowed.load(std::memory_order_relaxed));
    for (unsigned i = 0; i < kMaxLiveRecords; ++i) {
        const uintptr_t ptr = g_records[i].ptr.load(std::memory_order_acquire);
        if (ptr == 0) continue;
        std::fprintf(out, "[host-res] pid=%d   live 0x%llx size=%zu\n",
                     getpid(), static_cast<unsigned long long>(ptr),
                     g_records[i].size);
        for (unsigned f = 0; f < g_records[i].frame_count; ++f) {
            void* addr = g_records[i].frames[f];
            Dl_info info;
            std::memset(&info, 0, sizeof(info));
            if (dladdr(addr, &info) != 0 && info.dli_sname != nullptr) {
                std::fprintf(out, "[host-res] pid=%d     #%u %p %s +%lld\n",
                             getpid(), f, addr, info.dli_sname,
                             static_cast<long long>(
                                 reinterpret_cast<const char*>(addr) -
                                 reinterpret_cast<const char*>(info.dli_saddr)));
            } else {
                std::fprintf(out, "[host-res] pid=%d     #%u %p\n",
                             getpid(), f, addr);
            }
        }
    }
}

void dump_section(std::FILE* out) {
    dump_host_resources(out);
}

}  // namespace

/// Physical footprint is the ledger macOS itself uses for memory pressure and
/// for Activity Monitor's "Memory" column. Unlike resident_size it counts a
/// physical page once even when the task maps it twice, so it is the honest
/// measure for a copy-on-write snapshot.
void dump_task_footprint(std::FILE* out) {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO,
                  reinterpret_cast<task_info_t>(&info), &count)
        != KERN_SUCCESS) {
        return;
    }
    std::fprintf(out,
                 "[host-res] pid=%d phys_footprint=%llu resident=%llu "
                 "compressed=%llu virtual=%llu\n",
                 getpid(),
                 static_cast<unsigned long long>(info.phys_footprint),
                 static_cast<unsigned long long>(info.resident_size),
                 static_cast<unsigned long long>(info.compressed),
                 static_cast<unsigned long long>(info.virtual_size));
}

void dump_host_resources(std::FILE* out) {
    if (out == nullptr) return;
    dump_task_footprint(out);
    dump_mach_ports(out);
    dump_large_allocs(out);
    std::fflush(out);
}

void install_host_resource_probe() {
    static std::atomic<bool> installed{false};
    if (installed.exchange(true)) return;

    const char* gate = std::getenv("VITREO_LARGE_ALLOC_PROBE");
    if (gate != nullptr && gate[0] == '1') {
        if (const char* bytes = std::getenv("VITREO_LARGE_ALLOC_BYTES")) {
            const long long parsed = std::atoll(bytes);
            if (parsed > 0) {
                g_threshold.store(static_cast<std::size_t>(parsed),
                                  std::memory_order_relaxed);
            }
        }
        malloc_logger = &large_alloc_logger;
    }

    register_hang_dump_section(&dump_section);
}

}  // namespace vitreo::diagnostics
