// SPDX-License-Identifier: LGPL-2.1-only
// Production REP scan helpers, without process/dispatcher launch costs.
// First-call and warm costs are separate. Exact mapping tails deliberately
// exercise strlen-style unbounded counts on small readable mappings.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <vector>

#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/jit/jit_helpers.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using Clock = std::chrono::steady_clock;

namespace {
enum class Stop { Early, End, Absent };
const char* stop_name(Stop stop) {
    return stop == Stop::Early ? "early" : stop == Stop::End ? "end" : "absent";
}
constexpr u8 kRax = static_cast<u8>(RegIndex::RAX);
constexpr u8 kRcx = static_cast<u8>(RegIndex::RCX);
constexpr u8 kRsi = static_cast<u8>(RegIndex::RSI);
constexpr u8 kRdi = static_cast<u8>(RegIndex::RDI);

void reset(CpuState& cpu, GuestAddr src, GuestAddr dst, u64 count) {
    cpu.discard_lazy_flags();
    cpu.flags.rflags = 0x203; // Deliberate nonzero entry status, DF clear.
    cpu.pending_raise.active = false;
    cpu.rip = 0x1000;
    cpu.gpr.set64(kRax, 0x5A);
    cpu.gpr.set64(kRcx, count);
    cpu.gpr.set64(kRsi, src);
    cpu.gpr.set64(kRdi, dst);
}

u64 signature(CpuState& cpu, GuestAddr src, GuestAddr dst) {
    cpu.materialize_lazy_flags();
    // Normalize addresses so baseline/candidate and both bitnesses share a
    // stable observation. The full-width registers are validated separately.
    return (cpu.gpr.get64(kRcx) & 0xFFFFFFFFull) +
           3 * (cpu.gpr.get64(kRsi) - src) +
           5 * (cpu.gpr.get64(kRdi) - dst) +
           7 * cpu.flags.rflags + 11 * cpu.pending_raise.active;
}

bool measure(bool wide, bool cmps, usize length, Stop stop, bool unbounded,
             usize byte_budget, usize samples) {
    const usize mapping_bytes = (length + 4095) & ~usize{4095};
    const GuestAddr src_base = wide ? 0x100100000ull : 0x100000;
    const GuestAddr dst_base = wide ? 0x200100000ull : 0x400000;
    const GuestAddr src = src_base + mapping_bytes - length;
    const GuestAddr dst = dst_base + mapping_bytes - length;
    GuestMemory memory;
    if (!memory.map_region(0x1000, 0x1000, MemoryProtection::ReadWriteExecute) ||
        !memory.map_region(src_base, mapping_bytes, MemoryProtection::ReadWrite) ||
        !memory.map_region(dst_base, mapping_bytes, MemoryProtection::ReadWrite)) return false;
    std::vector<u8> a(length, 0x41), b(length, 0x41);
    if (stop != Stop::Absent) {
        const usize index = stop == Stop::Early ? 0 : length - 1;
        b[index] = cmps ? 0x42 : 0x5A;
    }
    const u8 code[] = {cmps ? u8{0xF3} : u8{0xF2}, cmps ? u8{0xA6} : u8{0xAE}};
    if (!memory.write_bytes(src, a.data(), a.size()) ||
        !memory.write_bytes(dst, b.data(), b.size()) ||
        !memory.write_bytes(0x1000, code, sizeof code)) return false;
    const u64 count = unbounded
        ? (wide ? std::numeric_limits<u64>::max() : std::numeric_limits<u32>::max())
        : length;
    const usize scanned = stop == Stop::Early ? 1 : length;
    const usize calls = std::clamp(byte_budget / scanned, usize{16}, usize{4096});
    CpuState actual;
    if (wide) actual.init_long_mode(); else actual.init_protected_mode();
    const auto helper = cmps ? &jit_repe_cmpsb : &jit_repne_scasb;
    reset(actual, src, dst, count);
    const auto first_begin = Clock::now();
    helper(&actual, &memory);
    const double first_ns = std::chrono::duration<double, std::nano>(Clock::now() - first_begin).count();

    CpuState oracle;
    if (wide) oracle.init_long_mode(); else oracle.init_protected_mode();
    reset(oracle, src, dst, count);
    Interpreter interpreter(oracle, memory);
    const auto result = interpreter.step();
    const bool fault = unbounded && stop == Stop::Absent;
    bool okay = result == (fault ? ExecResult::Exception : ExecResult::Continue) &&
                actual.pending_raise.active == fault;
    actual.materialize_lazy_flags();
    oracle.materialize_lazy_flags();
    for (u8 reg = 0; reg < 16; ++reg)
        okay &= actual.gpr.get64(reg) == oracle.gpr.get64(reg);
    okay &= actual.flags.rflags == oracle.flags.rflags;
    okay &= actual.gpr.get64(kRcx) == count - scanned;
    okay &= actual.gpr.get64(kRdi) == dst + scanned;
    okay &= actual.gpr.get64(kRsi) == src + (cmps ? scanned : 0);
    if (fault) {
        okay &= interpreter.context().exception_addr == (cmps ? src : dst) + length;
        okay &= actual.pending_raise.args[0] == 0;
        okay &= actual.pending_raise.args[1] == (cmps ? src : dst) + length;
    }
    // Interpreter faults live in its context rather than pending_raise;
    // normalize that representation before deriving the timed observation.
    oracle.pending_raise.active = fault;
    const u64 expected_signature = signature(oracle, src, dst);
    std::vector<double> timings;
    u64 checksum = 0;
    for (usize sample = 0; sample < samples; ++sample) {
        u64 observed = 0;
        const auto begin = Clock::now();
        for (usize call = 0; call < calls; ++call) {
            reset(actual, src, dst, count);
            helper(&actual, &memory);
            observed += signature(actual, src, dst);
        }
        const double ns = std::chrono::duration<double, std::nano>(Clock::now() - begin).count() / calls;
        okay &= observed == expected_signature * calls;
        timings.push_back(ns);
        checksum += observed;
    }
    std::sort(timings.begin(), timings.end());
    std::printf("{\"bits\":%u,\"operation\":\"%s\",\"length\":%zu,\"stop\":\"%s\","
                "\"unbounded\":%s,\"mapping_bytes\":%zu,\"memory_backend\":\"region\","
                "\"fault\":%s,\"valid\":%s,\"calls_per_sample\":%zu,\"samples\":%zu,"
                "\"first_call_ns\":%.3f,\"warm_ns_per_call\":%.3f,\"scanned_bytes\":%zu,"
                "\"signature\":%llu,\"checksum\":%llu}\n",
                wide ? 64 : 32, cmps ? "repe_cmpsb" : "repne_scasb", length, stop_name(stop),
                unbounded ? "true" : "false", mapping_bytes, fault ? "true" : "false",
                okay ? "true" : "false", calls, samples, first_ns, timings[timings.size() / 2], scanned,
                static_cast<unsigned long long>(expected_signature), static_cast<unsigned long long>(checksum));
    if (!okay)
        std::fprintf(stderr, "REP helper state mismatch bits=%u cmps=%u length=%zu stop=%s unbounded=%u helper_flags=%llx oracle_flags=%llx\n",
                     wide ? 64 : 32, unsigned(cmps), length, stop_name(stop), unsigned(unbounded),
                     (unsigned long long)actual.flags.rflags, (unsigned long long)oracle.flags.rflags);
    return okay;
}
} // namespace

int main(int argc, char** argv) {
    const auto usage = [&] {
        std::fprintf(stderr, "usage: %s [byte_budget=1048576] [samples=5] [--skip-faults]\n", argv[0]);
    };
    if (argc == 2 && std::string_view(argv[1]) == "--help") { usage(); return 0; }
    const usize byte_budget = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1u << 20;
    const usize samples = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 5;
    const bool skip_faults = argc > 3 && std::string_view(argv[3]) == "--skip-faults";
    if (!byte_budget || !samples || samples > 31 || argc > 4 || (argc > 3 && !skip_faults)) {
        usage();
        return 2;
    }
    std::fprintf(stderr, "[rep-scan-bench] unbounded-absent fault cases %s; all selected rows require exact state\n",
                 skip_faults ? "omitted symmetrically" : "included");
    bool okay = true;
    for (const bool wide : {false, true})
        for (const bool cmps : {false, true})
            for (const usize length : {16u, 256u, 4096u, 65536u})
                for (const auto stop : {Stop::Early, Stop::End, Stop::Absent})
                    for (const bool unbounded : {false, true})
                        if (!(skip_faults && unbounded && stop == Stop::Absent))
                            okay &= measure(wide, cmps, length, stop, unbounded, byte_budget, samples);
    return okay ? 0 : 3;
}
