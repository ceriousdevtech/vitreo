// SPDX-License-Identifier: LGPL-2.1-only

/// @file main_stack_relocation.cpp
/// @brief See main_stack_relocation.hpp for why this exists.

#include "core/memory/main_stack_relocation.hpp"

#include <crt_externs.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

extern char** environ;

namespace vitreo::memory {

namespace {

// Handed to the trampoline through globals, not registers or the stack: the
// old stack is unmapped before body() runs, so nothing may be read from it
// after the switch.
int    g_argc = 0;
char** g_argv = nullptr;
int  (*g_body)(int, char**) = nullptr;
std::uintptr_t g_old_rsp = 0;

// argv/environ strings live on the main thread stack on Darwin. Copy them
// onto the heap before the old stack is released, otherwise every later
// argv/getenv walk is a use-after-unmap.
std::vector<std::string> g_argv_store;
std::vector<char*>       g_argv_ptrs;
std::vector<std::string> g_env_store;
std::vector<char*>       g_env_ptrs;

void preserve_argv_environ(int argc, char** argv) {
    g_argv_store.clear();
    g_argv_ptrs.clear();
    if (argc < 0) argc = 0;
    g_argv_store.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        g_argv_store.emplace_back(argv[i] != nullptr ? argv[i] : "");
    }
    g_argv_ptrs.reserve(g_argv_store.size() + 1);
    for (auto& s : g_argv_store) g_argv_ptrs.push_back(s.data());
    g_argv_ptrs.push_back(nullptr);
    g_argc = argc;
    g_argv = g_argv_ptrs.data();
    if (char*** nsargv = _NSGetArgv()) *nsargv = g_argv;
    if (int* nsargc = _NSGetArgc()) *nsargc = argc;

    g_env_store.clear();
    g_env_ptrs.clear();
    char*** nsenviron = _NSGetEnviron();
    char** env = nsenviron != nullptr ? *nsenviron : environ;
    if (env != nullptr) {
        for (char** e = env; *e != nullptr; ++e) {
            g_env_store.emplace_back(*e);
        }
    }
    g_env_ptrs.reserve(g_env_store.size() + 1);
    for (auto& s : g_env_store) g_env_ptrs.push_back(s.data());
    g_env_ptrs.push_back(nullptr);
    if (nsenviron != nullptr) *nsenviron = g_env_ptrs.data();
    environ = g_env_ptrs.data();
}

/// Region containing @p rsp, plus any mapping packed against its low edge
/// (the PROT_NONE stack guard). Stops at a gap so dyld is not swept up.
bool stack_span_containing(std::uintptr_t rsp, mach_vm_address_t* out_lo,
                           mach_vm_size_t* out_size) {
    mach_vm_address_t addr = rsp;
    mach_vm_size_t sz = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t kr = mach_vm_region(
        mach_task_self(), &addr, &sz, VM_REGION_BASIC_INFO_64,
        reinterpret_cast<vm_region_info_t>(&info), &count, &object);
    if (kr != KERN_SUCCESS || sz == 0 || addr >= 0x100000000ull) return false;

    mach_vm_address_t lo = addr;
    mach_vm_size_t total = sz;

    for (;;) {
        if (lo < 0x1000) break;
        mach_vm_address_t probe = lo - 1;
        mach_vm_address_t raddr = probe;
        mach_vm_size_t rsz = 0;
        vm_region_basic_info_data_64_t rinfo{};
        count = VM_REGION_BASIC_INFO_COUNT_64;
        object = MACH_PORT_NULL;
        kr = mach_vm_region(
            mach_task_self(), &raddr, &rsz, VM_REGION_BASIC_INFO_64,
            reinterpret_cast<vm_region_info_t>(&rinfo), &count, &object);
        if (kr != KERN_SUCCESS || rsz == 0) break;
        if (raddr + rsz != lo) break;          // gap — not part of this stack
        if (raddr >= 0x100000000ull) break;
        lo = raddr;
        total += rsz;
    }

    *out_lo = lo;
    *out_size = total;
    return true;
}

kern_return_t release_span(mach_vm_address_t lo, mach_vm_size_t size) {
    kern_return_t kr = mach_vm_deallocate(mach_task_self(), lo, size);
    if (kr == KERN_SUCCESS) return kr;

    // The main-thread stack is a special VM object on some Darwin versions
    // and deallocate can refuse it. Replacing the mapping with a fresh
    // anonymous span, then deallocating that, drops the pin.
    void* p = mmap(reinterpret_cast<void*>(static_cast<std::uintptr_t>(lo)),
                   static_cast<size_t>(size), PROT_NONE,
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (p != reinterpret_cast<void*>(static_cast<std::uintptr_t>(lo))) {
        return kr;
    }
    return mach_vm_deallocate(mach_task_self(), lo, size);
}

/// Runs on the new stack. Must not touch anything left on the old one.
[[noreturn]] void high_stack_trampoline() {
    mach_vm_address_t lo = 0;
    mach_vm_size_t size = 0;
    kern_return_t kr = KERN_FAILURE;
    if (stack_span_containing(g_old_rsp, &lo, &size)) {
        kr = release_span(lo, size);
    }
    std::fprintf(stderr,
                 "[stack] released 0x%llx-0x%llx (%llu KiB) kr=%d\n",
                 static_cast<unsigned long long>(lo),
                 static_cast<unsigned long long>(lo + size),
                 static_cast<unsigned long long>(size / 1024),
                 static_cast<int>(kr));
    std::fflush(stderr);

    const int rc = g_body(g_argc, g_argv);
    // _exit, not exit: atexit handlers and static destructors would run on
    // this stack expecting the process's original one, and some of them walk
    // it. But _exit skips stdio flushing, which silently swallows everything
    // the run printed -- so flush explicitly first.
    std::fflush(nullptr);
    _exit(rc);
}

} // namespace

[[noreturn]] void run_main_on_high_stack(int argc, char** argv,
                                         int (*body)(int, char**)) {
    g_body = body;

#if !defined(VITREO_ARCH_X86_64)
    // Guest addresses are translated on ARM64; nothing competes for the low
    // 4GB, so the stack can stay where it is. Callers on ARM64 should invoke
    // body() directly; this path exists so the symbol still links.
    { const int rc = body(argc, argv); std::fflush(nullptr); _exit(rc); }
#else
    std::uintptr_t old_rsp = 0;
    __asm__ volatile("movq %%rsp, %0" : "=r"(old_rsp));
    g_old_rsp = old_rsp;

    // Only worth moving if the stack is actually in the guest's way.
    if (old_rsp >= 0x100000000ull) {
        std::fprintf(stderr,
                     "[stack] rsp=0x%llx already above 4GB; leaving it\n",
                     static_cast<unsigned long long>(old_rsp));
        std::fflush(stderr);
        const int rc = body(argc, argv);
        std::fflush(nullptr);
        _exit(rc);
    }

    // A fresh stack well above the 4GB guest window. Not MAP_FIXED: any high
    // address will do, and forcing one risks evicting something.
    void* const region = mmap(reinterpret_cast<void*>(0x500000000ull),
                              kHighStackSize + kPageGuard,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANON, -1, 0);
    if (region == MAP_FAILED ||
        reinterpret_cast<std::uintptr_t>(region) < 0x100000000ull) {
        std::fprintf(stderr,
                     "[stack] could not allocate a high stack; large 32-bit "
                     "guests will not load natively\n");
        if (region != MAP_FAILED) {
            (void)munmap(region, kHighStackSize + kPageGuard);
        }
        const int rc = body(argc, argv);
        std::fflush(nullptr);
        _exit(rc);
    }

    (void)mprotect(region, kPageGuard, PROT_NONE);

    // Copy argv/environ off the old stack before RSP moves. The trampoline
    // unmaps that span unconditionally.
    preserve_argv_environ(argc, argv);

    auto* const top = static_cast<u8*>(region) + kPageGuard + kHighStackSize;
    const auto aligned = reinterpret_cast<std::uintptr_t>(top) & ~std::uintptr_t{15};

    std::fprintf(stderr,
                 "[stack] moving main thread off rsp=0x%llx onto 0x%llx\n",
                 static_cast<unsigned long long>(old_rsp),
                 static_cast<unsigned long long>(aligned));
    std::fflush(stderr);

    __asm__ volatile(
        "movq   %0, %%rsp\n\t"
        "xorq   %%rbp, %%rbp\n\t"   // terminate the frame chain: unwinders and
                                    // the crash handler must not walk into the
                                    // stack we are about to unmap
        "callq  *%1\n\t"
        :
        : "r"(aligned), "r"(&high_stack_trampoline)
        : "memory", "cc");
    __builtin_unreachable();
#endif
}

} // namespace vitreo::memory
