// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#include "runtime/diagnostics/pc_trace.hpp"
#include "runtime/diagnostics/hang_dump.hpp"

#include "core/memory/guest_memory.hpp"
#include <vitreo/diagnostics.hpp>
#include "winapi/kernel32/thread/thread_suspend.hpp"

#include <atomic>
#include <functional>
#include <string>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace vitreo::diagnostics {

namespace {

std::atomic<int> g_dump_requested{0};

std::mutex g_symbolizer_mu;
std::function<std::string(u64)> g_symbolizer;

/// "  (kernel32.dll!WaitForSingleObject)" for an address the resolver names,
/// empty otherwise. Called off signal context, from the servicing thread.
std::string symbolize(u64 addr) {
    std::function<std::string(u64)> fn;
    {
        std::lock_guard<std::mutex> lk(g_symbolizer_mu);
        fn = g_symbolizer;
    }
    if (!fn) return {};
    std::string name = fn(addr);
    if (name.empty()) return {};
    return " <" + name + ">";
}

/// Signal handler. Does nothing but record the request: walking the thread
/// registry needs a mutex and stdio, neither of which is async-signal-safe, and
/// the signal can land on a thread already holding that mutex. The servicing
/// thread below does the real work outside signal context.
void request_dump(int) noexcept {
    g_dump_requested.store(1, std::memory_order_relaxed);
}

void print_samples(unsigned round, std::FILE* extra) {
    auto& registry = winapi::ThreadSuspendRegistry::instance();
    const auto samples = registry.sample_threads();
    GuestMemory* memory = registry.guest_memory();
    auto emit = [&](std::FILE* out) {
        if (samples.empty()) {
            std::fprintf(out,
                         "[hang-dump] round=%u no published guest threads\n",
                         round);
            return;
        }
        for (const auto& sample : samples) {
            std::fprintf(out,
                         "[hang-dump] round=%u tid=%u rip=0x%llx sp=0x%llx "
                         "bp=0x%llx eax=0x%llx ecx=0x%llx edx=0x%llx "
                         "ebx=0x%llx esi=0x%llx edi=0x%llx mode=%s%s%s "
                         "suspend_depth=%d hostcall=%d/%d base=%d\n",
                         round, sample.tid,
                         static_cast<unsigned long long>(sample.rip),
                         static_cast<unsigned long long>(sample.sp),
                         static_cast<unsigned long long>(sample.bp),
                         static_cast<unsigned long long>(sample.eax),
                         static_cast<unsigned long long>(sample.ecx),
                         static_cast<unsigned long long>(sample.edx),
                         static_cast<unsigned long long>(sample.ebx),
                         static_cast<unsigned long long>(sample.esi),
                         static_cast<unsigned long long>(sample.edi),
                         sample.is_64bit ? "x64" : "x86",
                         sample.parked ? " parked" : "",
                         sample.in_host_call ? " in-host-call" : "",
                         sample.suspend_depth, sample.host_call_depth,
                         sample.guest_override_depth,
                         sample.guest_base_depth);
            if (const std::string named = symbolize(sample.rip); !named.empty()) {
                std::fprintf(out, "[hang-dump] round=%u tid=%u in%s\n",
                             round, sample.tid, named.c_str());
            }
            if (memory == nullptr || sample.sp == 0) continue;
            const u32 width = sample.is_64bit ? 8u : 4u;
            std::fprintf(out, "[hang-dump] round=%u tid=%u stack=",
                         round, sample.tid);
            for (unsigned i = 0; i < 8; ++i) {
                const GuestAddr addr = static_cast<GuestAddr>(
                    sample.sp + static_cast<u64>(i) * width);
                const u64 value = sample.is_64bit
                    ? memory->read_u64(addr).value_or(0)
                    : memory->read_u32(addr).value_or(0);
                std::fprintf(out, "%s0x%llx", i == 0 ? "" : ",",
                             static_cast<unsigned long long>(value));
            }
            std::fputc('\n', out);

            // Frame-pointer chains are absent in shipping browser and game
            // builds, so the "frames=" line below is usually empty. Scan the
            // stack for qwords
            // that point just past a CALL instead — the same technique as the
            // OOM backtrace — which yields a usable caller chain for a thread
            // that is spinning in guest code with no host call to name it.
            if (sample.is_64bit) {
                // Callee-saved pointers and what they point at, read as
                // doubles. A geometry pass may carry a 3-double vector in
                // r14/r15, and NaN, Inf, or absurd coordinates are what a
                // runaway physics loop looks like from the outside.
                std::fprintf(out, "[hang-dump] round=%u tid=%u r12=0x%llx "
                             "r13=0x%llx r14=0x%llx r15=0x%llx\n",
                             round, sample.tid,
                             (unsigned long long)sample.r12,
                             (unsigned long long)sample.r13,
                             (unsigned long long)sample.r14,
                             (unsigned long long)sample.r15);
                const struct { const char* n; u64 a; } ptrs[] = {
                    {"r12", sample.r12}, {"r13", sample.r13},
                    {"r14", sample.r14}, {"r15", sample.r15}};
                for (const auto& pr : ptrs) {
                    if (pr.a < 0x10000 || pr.a >= 0x0000800000000000ull) continue;
                    double d[3]; bool ok = true;
                    for (int i = 0; i < 3 && ok; ++i) {
                        auto v = memory->read_u64(pr.a + 8u * i);
                        if (!v.is_ok()) { ok = false; break; }
                        u64 bits = v.value(); std::memcpy(&d[i], &bits, 8);
                    }
                    if (ok) std::fprintf(out, "[hang-dump] round=%u tid=%u [%s]="
                                         "(%.6g, %.6g, %.6g)\n", round, sample.tid,
                                         pr.n, d[0], d[1], d[2]);
                }
                std::fprintf(out, "[hang-dump] round=%u tid=%u rets=",
                             round, sample.tid);
                int found = 0;
                for (u64 off = 0; off < 0x1000 && found < 16; off += 8) {
                    auto v = memory->read_u64(sample.sp + off);
                    if (!v.is_ok()) break;
                    const u64 a = v.value();
                    if (a < 0x10000 || a >= 0x0000800000000000ull) continue;
                    auto b5 = memory->read_u8(a - 5);   // E8 rel32
                    auto b6 = memory->read_u8(a - 6);   // FF 15 [rip+d32]
                    auto b6b = memory->read_u8(a - 5);
                    auto b2 = memory->read_u8(a - 2);   // FF Dx call reg
                    const bool is_ret =
                        (b5.is_ok() && b5.value() == 0xE8) ||
                        (b6.is_ok() && b6.value() == 0xFF && b6b.is_ok() &&
                         (b6b.value() == 0x15 || (b6b.value() & 0xF8) == 0x90)) ||
                        (b2.is_ok() && b2.value() == 0xFF);
                    if (!is_ret) continue;
                    std::fprintf(out, "%s0x%llx", found == 0 ? "" : ",",
                                 static_cast<unsigned long long>(a));
                    ++found;
                }
                std::fputc('\n', out);
            }

            GuestAddr frame = static_cast<GuestAddr>(sample.bp);
            std::fprintf(out, "[hang-dump] round=%u tid=%u frames=",
                         round, sample.tid);
            for (unsigned i = 0; i < 6 && frame != 0; ++i) {
                const u64 next = sample.is_64bit
                    ? memory->read_u64(frame).value_or(0)
                    : memory->read_u32(frame).value_or(0);
                const u64 ret = sample.is_64bit
                    ? memory->read_u64(frame + width).value_or(0)
                    : memory->read_u32(frame + width).value_or(0);
                std::fprintf(out, "%s0x%llx%s", i == 0 ? "" : ",",
                             static_cast<unsigned long long>(ret),
                             symbolize(ret).c_str());
                if (next <= frame || next - frame > 16 * 1024 * 1024) break;
                frame = static_cast<GuestAddr>(next);
            }
            std::fputc('\n', out);
        }
    };
    emit(stderr);
    if (extra != nullptr) {
        emit(extra);
    }
    // A spinning thread never faults, so the PCTRACE ring would otherwise
    // only be seen on a crash. The hang dump is exactly when it is wanted.
    if (::vitreo::cli::pctrace_enabled()) {
        vitreo_pctrace_dump_ring("hang-dump");
    }
}

std::mutex g_extra_mu;
std::vector<void (*)(std::FILE*)> g_extra_dumpers;

void run_extra_dumpers(std::FILE* out) {
    std::vector<void (*)(std::FILE*)> fns;
    {
        std::lock_guard<std::mutex> lk(g_extra_mu);
        fns = g_extra_dumpers;
    }
    for (auto* fn : fns) {
        if (out != nullptr) fn(out);
        fn(stderr);
    }
}

void service_loop() {
    for (;;) {
        if (g_dump_requested.exchange(0, std::memory_order_relaxed) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        // Three rounds spaced apart: one round says where each thread is, and
        // the spread across rounds says whether it is going anywhere.
        std::FILE* extra = std::fopen("/tmp/vitreo-hang-dump.txt", "w");
        auto banner = [&](std::FILE* out) {
            if (out == nullptr) return;
            std::fprintf(out, "[hang-dump] === guest thread positions ===\n");
        };
        banner(stderr);
        banner(extra);
        for (unsigned round = 0; round < 3; ++round) {
            if (round != 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            print_samples(round, extra);
        }
        run_extra_dumpers(extra);
        // With hostcall capture on (/tmp/vitreo-hostcall-ring), the recent API
        // calls say what a parked process was last doing; a process that
        // never exits otherwise never prints its ring.
        vitreo::diagnostics::dump_hostcalls("hang-dump (SIGUSR1)");
        auto footer = [&](std::FILE* out) {
            if (out == nullptr) return;
            std::fprintf(out, "[hang-dump] === end ===\n");
            std::fflush(out);
        };
        footer(stderr);
        footer(extra);
        if (extra != nullptr) {
            std::fclose(extra);
        }
    }
}

}  // namespace

void set_hang_dump_symbolizer(std::function<std::string(u64)> fn) {
    std::lock_guard<std::mutex> lk(g_symbolizer_mu);
    g_symbolizer = std::move(fn);
}

void register_hang_dump_section(void (*fn)(std::FILE*)) {
    if (fn == nullptr) return;
    std::lock_guard<std::mutex> lk(g_extra_mu);
    g_extra_dumpers.push_back(fn);
}

void install_hang_dump() {
    static std::atomic<bool> installed{false};
    if (installed.exchange(true)) return;

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = request_dump;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, nullptr);
#ifdef SIGINFO
    sigaction(SIGINFO, &sa, nullptr);
#endif

    std::thread(service_loop).detach();
}

}  // namespace vitreo::diagnostics
