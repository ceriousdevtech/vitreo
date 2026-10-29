// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#include <core/cpu/jit/jit_symbol_map.hpp>
#include <core/cpu/jit/jit_block.hpp>
#include <core/cpu/jit/hot_block_dump.hpp>
#include <core/cpu/jit/jit_provenance.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_set>
#include <unistd.h>
#include <ctime>

namespace vitreo::cpu::jit::symbol_map {

namespace {

std::mutex g_mutex;
std::FILE*  g_file = nullptr;
// VITREO_JIT_SYMBOL_MAP_RIPS: when set, only blocks starting at one of these
// guest RIPs are recorded (with their code, if requested).
std::unordered_set<u64>* g_rip_filter = nullptr;

// 0 unknown, 1 off, 2 on. Read on every compile, so the disabled answer must
// be a plain relaxed load rather than a cross-TU call into a static
// initialiser.
std::atomic<int> g_state{0};

u64 monotonic_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<u64>(ts.tv_sec) * 1000000000ull + static_cast<u64>(ts.tv_nsec);
}

ProvenanceWriter* provenance() {
    static ProvenanceWriter* const writer = []() -> ProvenanceWriter* {
        const char* path = std::getenv("VITREO_JIT_PROVENANCE");
        if (path == nullptr || *path == '\0') return nullptr;
        // A process-specific new file prevents histories from distinct runs
        // being mistaken for one another. Never truncate an existing history.
        std::string expanded = hot_dump::expand_process_path(path, static_cast<u64>(::getpid()));
        expanded += "." + std::to_string(::getpid());
        std::FILE* file = std::fopen(expanded.c_str(), "wxe");
        if (file == nullptr) {
            std::fprintf(stderr, "[jit-provenance] cannot create %s; disabled\n", expanded.c_str());
            return nullptr;
        }
        std::setvbuf(file, nullptr, _IOFBF, 65536);
        u64 capacity = 262144;
        if (const char* limit = std::getenv("VITREO_JIT_PROVENANCE_CAP")) {
            char* end = nullptr;
            const auto requested = std::strtoull(limit, &end, 10);
            if (end != limit && *end == '\0' && requested > 0 && requested <= 1048576)
                capacity = requested;
        }
        return new ProvenanceWriter(file, capacity, static_cast<u64>(::getpid()), monotonic_ns());
    }();
    return writer;
}

[[nodiscard]] bool init_slow() noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    const int seen = g_state.load(std::memory_order_relaxed);
    if (seen != 0) return seen == 2;

    const char* path = std::getenv("VITREO_JIT_SYMBOL_MAP");
    if (path == nullptr || path[0] == '\0') {
        g_state.store(1, std::memory_order_relaxed);
        return false;
    }
    // Appended, not truncated: a guest that re-execs as an app bundle runs
    // more than one process against the same path, and losing the first
    // process's blocks would resolve its samples to the second's addresses.
    // The pid column keeps them apart.
    // "%p" expands to the host pid, so every process of a guest tree that
    // inherits the variable writes its own map.
    const std::string expanded =
        hot_dump::expand_process_path(path, static_cast<u64>(::getpid()));
    if (const char* rips = std::getenv("VITREO_JIT_SYMBOL_MAP_RIPS");
        rips != nullptr && rips[0] != '\0') {
        std::FILE* list = std::fopen(rips, "re");
        if (list == nullptr) {
            std::fprintf(stderr,
                         "[jit-symbols] cannot open %s; symbol map disabled\n",
                         rips);
            g_state.store(1, std::memory_order_relaxed);
            return false;
        }
        g_rip_filter = new std::unordered_set<u64>();
        unsigned long long rip = 0;
        // One hex RIP per line; anything after it on the line is ignored.
        while (std::fscanf(list, "%llx%*[^\n]", &rip) == 1) {
            g_rip_filter->insert(rip);
        }
        std::fclose(list);
    }
    g_file = std::fopen(expanded.c_str(), "ae");
    if (g_file == nullptr) {
        std::fprintf(stderr,
                     "[jit-symbols] cannot open %s; symbol map disabled\n",
                     path);
        g_state.store(1, std::memory_order_relaxed);
        return false;
    }
    std::fprintf(g_file, "# pid=%d host_start size guest_rip link_off "
                         "call_off native_insns\n",
                 static_cast<int>(getpid()));
    g_state.store(2, std::memory_order_relaxed);
    return true;
}

}  // namespace

bool enabled() noexcept {
    const int seen = g_state.load(std::memory_order_relaxed);
    if (seen != 0) return seen == 2;
    return init_slow();
}

bool wants_sites(u64 guest_addr) noexcept {
    static const bool with_code =
        std::getenv("VITREO_JIT_SYMBOL_MAP_CODE") != nullptr;
    if (!enabled() || !with_code) return false;
    return g_rip_filter == nullptr || g_rip_filter->count(guest_addr) != 0;
}

void record_site(u32 host_begin, u32 host_end, const char* opcode) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file == nullptr) return;
    std::fprintf(g_file, "site %x %x %s\n", host_begin, host_end, opcode);
}

void record_provenance(const CompiledBlock& block) noexcept {
    if (block.code != nullptr && block.code_size != 0) {
        if (auto* history = provenance()) {
            std::lock_guard<std::mutex> lock(g_mutex);
            history->record(monotonic_ns(), block.guest_addr,
                reinterpret_cast<uintptr_t>(block.code), block.code_size,
                reinterpret_cast<uintptr_t>(block.cold_code()), block.cold_size);
        }
    }
}

void record(const CompiledBlock& block) noexcept {
    if (!enabled()) return;
    if (block.code == nullptr || block.code_size == 0) return;
    if (g_rip_filter != nullptr && g_rip_filter->count(block.guest_addr) == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file == nullptr) return;
    std::fprintf(g_file, "%llx %x %llx %x %x %u\n",
                 static_cast<unsigned long long>(
                     reinterpret_cast<uintptr_t>(block.code)),
                 block.code_size,
                 static_cast<unsigned long long>(block.guest_addr),
                 block.link_code_offset,
                 block.call_code_offset,
                 block.native_insns);
    // Optionally the emitted words too. A sample resolves to
    // (block, offset), and the last mile from there to "which instruction" is
    // a disassembly -- which cannot be taken after the fact, because the arena
    // is gone and its chunks may have been recycled.
    static const bool with_code =
        std::getenv("VITREO_JIT_SYMBOL_MAP_CODE") != nullptr;
    if (with_code) {
        std::fprintf(g_file, "code %llx ",
                     static_cast<unsigned long long>(
                         reinterpret_cast<uintptr_t>(block.code)));
        const auto* words = static_cast<const u32*>(block.code);
        for (u32 i = 0; i < block.code_size / sizeof(u32); ++i) {
            std::fprintf(g_file, "%08x", words[i]);
        }
        std::fputc('\n', g_file);
        // A split-placed block's cold part, at its own address.
        if (const u8* cold_bytes = block.cold_code()) {
            std::fprintf(g_file, "cold %llx ",
                         static_cast<unsigned long long>(
                             reinterpret_cast<uintptr_t>(cold_bytes)));
            const auto* cold = reinterpret_cast<const u32*>(cold_bytes);
            for (u32 i = 0; i < block.cold_size / sizeof(u32); ++i) {
                std::fprintf(g_file, "%08x", cold[i]);
            }
            std::fputc('\n', g_file);
        }
    }

    // Flushed per block on purpose. The interesting runs are the ones that end
    // in a kill, and a buffered map loses exactly the blocks that were hot.
    std::fflush(g_file);
}

void flush_provenance(std::FILE* diagnostic) noexcept {
    auto* history = provenance();
    if (history == nullptr) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    const bool ok = history->flush(monotonic_ns());
    if (diagnostic != nullptr) std::fprintf(diagnostic,
        "[jit-provenance] pid=%d count=%llu loss=%d flush_ok=%d\n",
        static_cast<int>(::getpid()), static_cast<unsigned long long>(history->count()),
        history->lost() ? 1 : 0, ok ? 1 : 0);
}

}  // namespace vitreo::cpu::jit::symbol_map
