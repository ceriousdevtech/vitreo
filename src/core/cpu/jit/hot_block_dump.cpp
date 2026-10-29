// SPDX-License-Identifier: LGPL-2.1-only
#include "hot_block_dump.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <cstdio>
#include <unistd.h>

#if defined(__APPLE__)
#include <pthread.h>
#endif

namespace vitreo::cpu::jit::hot_dump {
namespace {

struct Entry {
    std::vector<u8> bytes;
    u32 native_insns = 0;
    bool delegated = false;
    u64 exec_count = 0;
    u64 insn_count = 0;
};

struct RipEntries {
    std::vector<Entry> variants;
};

std::mutex g_mu;
std::unordered_map<u64, RipEntries> g_blocks;
using detail::g_state;
std::atomic<bool> g_dumped{false};
std::string g_path;
u32 g_top_n = 64;
u64 g_target_tid = 0;

[[nodiscard]] u64 current_tid() noexcept {
#if defined(__APPLE__)
    static thread_local u64 tid = [] {
        u64 value = 0;
        (void)pthread_threadid_np(nullptr, &value);
        return value;
    }();
    return tid;
#else
    static thread_local const u64 tid =
        static_cast<u64>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    return tid;
#endif
}

[[nodiscard]] bool selected_thread() noexcept {
    return g_target_tid == 0 || current_tid() == g_target_tid;
}

const char* classify(const std::vector<u8>& bytes) {
    bool sse = false;
    bool fpu = false;
    for (size_t i = 0; i + 1 < bytes.size(); ++i) {
        if (bytes[i] == 0x0F &&
            (bytes[i + 1] == 0x10 || bytes[i + 1] == 0x11 ||
             bytes[i + 1] == 0x28 || bytes[i + 1] == 0x29 ||
             bytes[i + 1] == 0x6F || bytes[i + 1] == 0x7F ||
             bytes[i + 1] == 0xEF || bytes[i + 1] == 0x57)) {
            sse = true;
        }
        if (bytes[i] >= 0xD8 && bytes[i] <= 0xDF) fpu = true;
    }
    if (sse) return "sse";
    if (fpu) return "x87";
    if (!bytes.empty() && (bytes[0] == 0xA4 || bytes[0] == 0xA5 ||
                           bytes[0] == 0xF3)) {
        return "string";
    }
    return "alu";
}

}  // namespace

namespace detail {

// Definition for the header's inline gate.
std::atomic<int> g_state{0};  // 0 unknown, 1 off, 2 on

[[gnu::noinline]] bool hot_dump_init_slow() noexcept {
    const char* p = std::getenv("VITREO_JIT_DUMP_HOT");
    if (!p || !*p) {
        g_state.store(1, std::memory_order_release);
        return false;
    }
    g_path = expand_process_path(p, static_cast<u64>(::getpid()));
    if (const char* n = std::getenv("VITREO_JIT_DUMP_HOT_N")) {
        const unsigned long v = std::strtoul(n, nullptr, 10);
        if (v > 0 && v < 10000) g_top_n = static_cast<u32>(v);
    }
    if (const char* tid = std::getenv("VITREO_JIT_DUMP_HOT_TID")) {
        char* end = nullptr;
        const unsigned long long value = std::strtoull(tid, &end, 0);
        if (end != tid && *end == '\0') g_target_tid = static_cast<u64>(value);
    }
    static std::once_flag once;
    std::call_once(once, [] { std::atexit(dump); });
    g_state.store(2, std::memory_order_release);
    return true;
}

}  // namespace detail

std::string expand_process_path(std::string_view path, u64 process_id) {
    const std::string pid = std::to_string(process_id);
    std::string expanded;
    expanded.reserve(path.size() + pid.size());
    size_t cursor = 0;
    while (cursor < path.size()) {
        const size_t token = path.find("%p", cursor);
        if (token == std::string_view::npos) {
            expanded.append(path.substr(cursor));
            break;
        }
        expanded.append(path.substr(cursor, token - cursor));
        expanded.append(pid);
        cursor = token + 2;
    }
    return expanded;
}

u32 record_compile(u64 rip, const u8* bytes, u32 len, u32 native_insns,
                   bool delegated) noexcept {
    if (!enabled() || !selected_thread() || !bytes || len == 0) {
        return kInvalidCodeVariant;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    auto& entries = g_blocks[rip];
    const auto same_bytes = [bytes, len](const Entry& entry) {
        return entry.bytes.size() == len &&
               std::memcmp(entry.bytes.data(), bytes, len) == 0;
    };
    const auto found = std::find_if(entries.variants.begin(),
                                    entries.variants.end(), same_bytes);
    if (found != entries.variants.end()) {
        return static_cast<u32>(std::distance(entries.variants.begin(), found));
    }
    Entry entry;
    entry.bytes.assign(bytes, bytes + len);
    entry.native_insns = native_insns;
    entry.delegated = delegated;
    entries.variants.push_back(std::move(entry));
    return static_cast<u32>(entries.variants.size() - 1);
}

namespace {

void record_exec_locked(u64 rip, u32 code_variant, u32 block_insns,
                        u32 sample_weight) noexcept {
    const auto it = g_blocks.find(rip);
    if (it == g_blocks.end() || code_variant >= it->second.variants.size()) {
        return;
    }
    Entry& entry = it->second.variants[code_variant];
    entry.exec_count += sample_weight;
    entry.insn_count += static_cast<u64>(block_insns) * sample_weight;
}

std::vector<detail::CapturedBlock> snapshot_locked() {
    std::vector<detail::CapturedBlock> snapshot;
    for (const auto& [rip, entries] : g_blocks) {
        for (size_t i = 0; i < entries.variants.size(); ++i) {
            const Entry& entry = entries.variants[i];
            snapshot.push_back(detail::CapturedBlock{
                rip,
                static_cast<u32>(i),
                entry.bytes,
                entry.native_insns,
                entry.delegated,
                entry.exec_count,
                entry.insn_count,
            });
        }
    }
    return snapshot;
}

}  // namespace

void record_exec(u64 rip, u32 code_variant, u32 block_insns) noexcept {
    if (!enabled() || !selected_thread()) return;
    // Dispatcher-only and sampled: a mutex on every entry made the dump
    // run too slow to reach the load-hot blocks. Ranking stays the same
    // shape; linked hops are still invisible (see cpu perf oracle docs).
    static thread_local u32 skip = 0;
    if ((++skip & 31u) != 0) return;
    std::lock_guard<std::mutex> lock(g_mu);
    record_exec_locked(rip, code_variant, block_insns, 32);
}

void dump() noexcept {
    if (g_state.load(std::memory_order_acquire) != 2) return;
    std::vector<detail::CapturedBlock> sorted;
    {
        // try_lock returns false on EBUSY/EINVAL and does not throw. lock()
        // would abort via noexcept if g_mu is held or already torn down.
        std::unique_lock<std::mutex> lock(g_mu, std::try_to_lock);
        if (!lock.owns_lock()) return;
        // Commit the one-shot only after acquiring the snapshot lock. A
        // transient race with record_exec must not permanently suppress the
        // later orderly/atexit snapshot.
        bool expected = false;
        if (!g_dumped.compare_exchange_strong(expected, true,
                                              std::memory_order_acq_rel)) {
            return;
        }
        sorted = snapshot_locked();
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) {
                  return a.insn_count > b.insn_count;
              });
    FILE* f = std::fopen(g_path.c_str(), "w");
    if (!f) {
        std::fprintf(stderr, "[jit-dump-hot] failed to write %s\n",
                     g_path.c_str());
        // The process may still reach an orderly shutdown with a writable
        // destination. Permit that retry; dump() remains try-lock-safe if the
        // mutex has already been torn down.
        g_dumped.store(false, std::memory_order_release);
        return;
    }
    std::fputs("[\n", f);
    u32 shown = 0;
    for (const auto& e : sorted) {
        if (shown >= g_top_n) break;
        if (e.bytes.empty()) continue;
        if (shown) std::fputs(",\n", f);
        std::fprintf(f,
                     "  {\"rip\":\"0x%llx\",\"code_variant\":%u,"
                     "\"exec_count\":%llu,"
                     "\"insn_count\":%llu,\"native_insns\":%u,"
                     "\"delegated\":%s,\"class\":\"%s\","
                     "\"accounting\":\"sampled_dispatcher\","
                     "\"sample_rate\":32,\"linked_hops_included\":false,"
                     "\"capture_tid\":%llu,\"bytes\":\"",
                     static_cast<unsigned long long>(e.rip), e.code_variant,
                     static_cast<unsigned long long>(e.exec_count),
                     static_cast<unsigned long long>(e.insn_count),
                     e.native_insns, e.delegated ? "true" : "false",
                     classify(e.bytes),
                     static_cast<unsigned long long>(g_target_tid));
        for (u8 b : e.bytes) std::fprintf(f, "%02x", b);
        std::fputs("\"}", f);
        ++shown;
    }
    std::fputs("\n]\n", f);
    const bool write_failed = std::ferror(f) != 0;
    const bool close_failed = std::fclose(f) != 0;
    if (write_failed || close_failed) {
        std::fprintf(stderr, "[jit-dump-hot] incomplete write to %s\n",
                     g_path.c_str());
        g_dumped.store(false, std::memory_order_release);
        return;
    }
    std::fprintf(stderr, "[jit-dump-hot] wrote %u blocks to %s\n", shown,
                 g_path.c_str());
}

namespace detail {

std::vector<CapturedBlock> snapshot_for_testing() {
    std::lock_guard<std::mutex> lock(g_mu);
    return snapshot_locked();
}

void reset_for_testing() noexcept {
    std::lock_guard<std::mutex> lock(g_mu);
    g_blocks.clear();
}

void record_exec_for_testing(u64 rip, u32 code_variant, u32 block_insns,
                             u32 sample_weight) noexcept {
    std::lock_guard<std::mutex> lock(g_mu);
    record_exec_locked(rip, code_variant, block_insns, sample_weight);
}

}  // namespace detail

}  // namespace vitreo::cpu::jit::hot_dump
