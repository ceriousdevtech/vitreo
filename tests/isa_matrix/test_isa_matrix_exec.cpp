// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_isa_matrix_exec.cpp
/// @brief The ISA matrix parity and hardware gates.
///
/// The compile-side gate (test_isa_matrix.cpp) proves a form is lowered
/// natively. This proves the lowering is RIGHT: the JIT must agree with the
/// interpreter on every executable form (P), and the interpreter must agree
/// with real x86 through the Rosetta oracle (H).
///
/// Both are ratcheted rather than absolute. A from-zero run of this sweep
/// surfaces a large backlog, and a hard-fail gate on the first run would be
/// permanently red, which is indistinguishable from no gate at all. The
/// ratchet fails the moment the count rises, which is the property that keeps
/// a new lowering from quietly dropping a form.

#include "isa_form_enum.hpp"
#include "isa_matrix_exec.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "oracle/oracle_contract.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

using vitreo::isa_matrix::ExecResult;
using vitreo::isa_matrix::FormCase;
using vitreo::isa_matrix::SkipReason;

/// Seeds per form. Each is a different input class (ordinary, all-ones/zero,
/// carry boundary + FP specials, denormals); a lowering that is right on one
/// and wrong on another is the recorded failure shape for much of the FP
/// surface.
constexpr unsigned kSeeds = 4;

/// A form whose whole seed set takes longer than this is pathological: the
/// median is microseconds, and the entire 90k-case sweep runs in ~6 s.
constexpr double kSlowFormMs = 250.0;

std::string op_name(const FormCase& fc) {
    const char* n = vitreo::cpu::opcode_name(fc.opcode);
    return n ? n : "(unnamed)";
}

struct Sweep {
    std::size_t executed = 0;
    std::size_t parity_failures = 0;
    std::size_t hardware_failures = 0;
    std::size_t oracle_cases = 0;
    std::size_t undefined_flag_cases = 0;
    /// Cases where exactly one engine faulted. Counted before the
    /// `!r.interp_ran` drop below, which is the whole point: a case the
    /// interpreter faults on never reaches the value comparison.
    std::size_t fault_failures = 0;
    std::vector<std::string> fault_examples;
    std::map<std::string, unsigned> fault_by_op;
    std::map<std::string, unsigned> skipped;
    std::vector<std::string> parity_examples;
    std::vector<std::string> hardware_examples;
    /// Forms whose four seeds took longer than kSlowFormMs.
    ///
    /// This exists because a correctness-preserving change can still be
    /// catastrophically wrong in a way no assertion sees. Replacing the
    /// 256-bit lane-split allowlist with a declared property compiled, passed
    /// the coverage ratchet, RAISED native coverage and left the CPU suite
    /// green — and took this sweep from 6 s to over ten minutes, because it
    /// made `MASKMOVDQU` with VEX.L=1 reachable. The slowdown was the only
    /// signal, and finding its cause meant bisecting the form index by hand.
    /// Now the sweep names the form itself.
    std::vector<std::string> slow_forms;
    double slowest_ms = 0.0;
    /// Failure counts per mnemonic, so a run says WHICH instructions diverge
    /// rather than only how many cases did. A single systematic modelling
    /// difference can account for thousands of cases, and a bare total makes
    /// it indistinguishable from a broad correctness problem.
    std::map<std::string, unsigned> parity_by_op;
    std::map<std::string, unsigned> hardware_by_op;
};

Sweep run_sweep() {
    Sweep s;
    const std::vector<FormCase> forms =
        vitreo::isa_matrix::enumerate_all_forms();
    // Bound for local triage only. Unset in CI: a sweep that silently covers
    // part of the ISA is the measurement failure this whole program exists to
    // remove, so the limit is opt-in and reported.
    std::size_t budget = forms.size();
    if (const char* env = std::getenv("VITREO_ISA_MATRIX_MAX_FORMS")) {
        budget = std::strtoul(env, nullptr, 10);
        std::printf("[isa-matrix-exec] BOUNDED RUN: %zu of %zu forms\n",
                    budget, forms.size());
    }
    // Restrict the sweep itself to one mnemonic, not just the examples it
    // prints. Adjudicating a single instruction against real hardware needs
    // this: the oracle costs ~22 ms per case, and the hardware column over the
    // whole matrix is dominated by systematic Rosetta modelling differences
    // (segment selectors read as 0, the PTEST flag and FNSTENV carve-outs
    // documented in oracle_contract.hpp), which drown out the one opcode under
    // investigation.
    const char* only = std::getenv("VITREO_ISA_MATRIX_ONLY");

    std::size_t seen = 0;
    const bool trace = std::getenv("VITREO_ISA_MATRIX_TRACE") != nullptr;
    for (const FormCase& fc : forms) {
        if (seen++ >= budget) break;
        if (only != nullptr && op_name(fc).find(only) == std::string::npos) {
            continue;
        }
        if (trace) {
            std::printf("[form %zu] %s\n", seen - 1, fc.text().c_str());
            std::fflush(stdout);
        }
        const SkipReason why = vitreo::isa_matrix::classify(fc);
        if (why != SkipReason::None) {
            ++s.skipped[vitreo::isa_matrix::skip_reason_name(why)];
            continue;
        }
        const auto form_start = std::chrono::steady_clock::now();
        for (unsigned seed = 0; seed < kSeeds; ++seed) {
            const ExecResult r = vitreo::isa_matrix::exec_form(fc, seed);
            if (!r.fault_diff.empty()) {
                ++s.fault_failures;
                ++s.fault_by_op[op_name(fc)];
                // Same reason as the offender-list cap: 25 names a trend,
                // but the DIRECTION split of a fault class only shows up if
                // you can see all of them. A capped sample showed 25/25 in
                // one direction for a set that was mostly the other.
                static const bool dump_all =
                    std::getenv("VITREO_ISA_MATRIX_DUMP") != nullptr;
                if (dump_all || s.fault_examples.size() < 25) {
                    s.fault_examples.push_back(fc.text() + " seed=" +
                                               std::to_string(seed) + ": " +
                                               r.fault_diff);
                }
            }
            if (!r.interp_ran) continue;
            ++s.executed;
            if (r.oracle_ran) ++s.oracle_cases;
            if (!r.undefined_flag_diff.empty()) ++s.undefined_flag_cases;
            if (!r.parity_diff.empty()) {
                ++s.parity_failures;
                ++s.parity_by_op[op_name(fc)];
                if (s.parity_examples.size() < 25) {
                    s.parity_examples.push_back(fc.text() + " seed=" +
                                                std::to_string(seed) + ": " +
                                                r.parity_diff);
                }
            }
            if (!r.hardware_diff.empty()) {
                ++s.hardware_failures;
                ++s.hardware_by_op[op_name(fc)];
                if (s.hardware_examples.size() < 25) {
                    s.hardware_examples.push_back(fc.text() + " seed=" +
                                                  std::to_string(seed) + ": " +
                                                  r.hardware_diff);
                }
            }
        }
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - form_start).count();
        s.slowest_ms = std::max(s.slowest_ms, ms);
        if (ms > kSlowFormMs && s.slow_forms.size() < 20) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), " [%.0f ms]", ms);
            s.slow_forms.push_back(fc.text() + buf);
        }
    }
    return s;
}

const Sweep& sweep() {
    static const Sweep s = run_sweep();
    return s;
}

/// Ratchet file: two integers, written by this test.
struct Limits {
    std::size_t parity = 0;
    std::size_t hardware = 0;
    std::size_t fault = 0;
};

const char* limits_path() {
#ifdef VITREO_ISA_MATRIX_EXEC_BASELINE
    return VITREO_ISA_MATRIX_EXEC_BASELINE;
#else
    return "";
#endif
}

bool read_limits(Limits* out) {
    std::ifstream in(limits_path());
    if (!in.good()) return false;
    std::string line;
    while (std::getline(in, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        // Match on the KEY only -- the text before the colon. Scanning the
        // whole line meant the free-text "note" field, which is written last
        // and mentions these words, overwrote all three limits with whatever
        // strtoul made of its prose (zero). A note is documentation; it must
        // not be able to move a gate. The failure is not always the safe
        // direction either: a note containing "fault: 999999" would LOOSEN
        // the ratchet silently.
        const std::string key = line.substr(0, colon);
        const std::size_t v = std::strtoul(line.c_str() + colon + 1, nullptr, 10);
        if (key.find("parity") != std::string::npos) out->parity = v;
        if (key.find("hardware") != std::string::npos) out->hardware = v;
        if (key.find("fault") != std::string::npos) out->fault = v;
    }
    return true;
}

void write_limits(const Sweep& s) {
    // Keep the recorded hardware number when this run did not exercise the
    // hardware column (it is opt-in, see isa_matrix_exec.cpp). Writing 0 for
    // "did not measure" would silently tighten a gate nobody ran, and the next
    // person who does run it would read the resulting failure as a regression
    // they caused.
    Limits prev;
    const bool had = read_limits(&prev);
    const std::size_t hw = (s.oracle_cases == 0 && had) ? prev.hardware
                                                        : s.hardware_failures;
    std::ofstream out(limits_path());
    out << "{\n  \"parity_failures\": " << s.parity_failures
        << ",\n  \"fault_failures\": " << s.fault_failures
        << ",\n  \"hardware_failures\": " << hw
        << (s.oracle_cases == 0
                ? ",\n  \"note\": \"hardware column not run; set "
                  "VITREO_ISA_MATRIX_HARDWARE=1\""
                : "")
        << "\n}\n";
}

std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) out += "\n  " + s;
    return out;
}

}  // namespace

TEST(IsaMatrixExec, SweepCoversTheExecutableInstructionSet) {
    const Sweep& s = sweep();
    std::printf("[isa-matrix-exec] %zu case(s) executed, %zu on hardware; "
                "%zu parity, %zu fault, %zu hardware failure(s); "
                "%zu undefined-flag difference(s)\n",
                s.executed, s.oracle_cases, s.parity_failures,
                s.fault_failures, s.hardware_failures,
                s.undefined_flag_cases);
    for (const auto& [why, n] : s.skipped) {
        std::printf("[isa-matrix-exec]   skipped %-18s %u form(s)\n",
                    why.c_str(), n);
    }
    const auto top = [](const char* label,
                        const std::map<std::string, unsigned>& m) {
        std::vector<std::pair<std::string, unsigned>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
            return a.second > b.second;
        });
        // 20 is enough to name a trend, but working a class DOWN needs the
        // whole list -- the tail is where the last few lowerings hide.
        const unsigned cap =
            std::getenv("VITREO_ISA_MATRIX_DUMP") != nullptr ? ~0u : 20u;
        unsigned shown = 0;
        for (const auto& [name, n] : v) {
            if (shown++ >= cap) break;
            std::printf("[isa-matrix-exec]   %s %-30s %u\n", label,
                        name.c_str(), n);
        }
    };
    top("parity  ", s.parity_by_op);
    top("fault   ", s.fault_by_op);
    top("hardware", s.hardware_by_op);
    if (std::getenv("VITREO_ISA_MATRIX_DUMP") != nullptr) {
        for (const std::string& e : s.parity_examples) {
            std::printf("[parity] %s\n", e.c_str());
        }
        for (const std::string& e : s.fault_examples) {
            std::printf("[fault] %s\n", e.c_str());
        }
        for (const std::string& e : s.hardware_examples) {
            std::printf("[hardware] %s\n", e.c_str());
        }
    }
    if (std::getenv("VITREO_ISA_MATRIX_ONLY") == nullptr) {
        EXPECT_GT(s.executed, 1000u)
            << "the execution sweep collapsed; a ratchet over an empty sweep "
               "cannot fail and is not a gate.";
    }

    std::string slow;
    for (const std::string& f : s.slow_forms) slow += "\n  " + f;
    EXPECT_TRUE(s.slow_forms.empty())
        << s.slow_forms.size() << " form(s) took over " << kSlowFormMs
        << " ms; slowest " << s.slowest_ms << " ms." << slow
        << "\n\nA pathological lowering shows up here as time long before it "
           "shows up as a wrong answer. Re-run with VITREO_ISA_MATRIX_TRACE=1 "
           "to see each form as it executes.";
}

/// JIT vs interpreter. The interpreter is the semantic reference, so any
/// difference is a JIT bug by definition.
TEST(IsaMatrixExec, JitMatchesInterpreterRatchet) {
    if (std::getenv("VITREO_ISA_MATRIX_ONLY") != nullptr) {
        GTEST_SKIP() << "filtered run: ratchet numbers are not comparable";
    }
    const Sweep& s = sweep();
    ASSERT_NE(limits_path()[0], '\0') << "baseline path not configured";
    if (std::getenv("VITREO_ISA_MATRIX_WRITE_BASELINE") != nullptr) {
        write_limits(s);
        GTEST_SKIP() << "baseline rewritten";
    }
    Limits lim;
    if (!read_limits(&lim)) {
        write_limits(s);
        GTEST_SKIP() << "no baseline yet; wrote one. Commit it.";
    }
    EXPECT_LE(s.parity_failures, lim.parity)
        << "JIT/interpreter divergences rose from " << lim.parity << " to "
        << s.parity_failures << ".\nExamples:" << join(s.parity_examples);

    // Fault parity is its own number because it is its own failure mode: the
    // two engines can agree on every register and still disagree on whether
    // the instruction raised at all. The value comparison cannot see that --
    // it only runs when both engines completed -- so folding this into the
    // parity count would let one hide behind the other.
    EXPECT_LE(s.fault_failures, lim.fault)
        << "fault divergences rose from " << lim.fault << " to "
        << s.fault_failures
        << ": one engine raised and the other did not.\nExamples:"
        << join(s.fault_examples);
}

/// Interpreter vs real x86. Skipped when the Rosetta oracle runner is absent,
/// which is the only honest thing to do — a green H gate with no oracle would
/// be a lie.
TEST(IsaMatrixExec, MatchesHardwareRatchet) {
    if (std::getenv("VITREO_ISA_MATRIX_ONLY") != nullptr) {
        GTEST_SKIP() << "filtered run: ratchet numbers are not comparable";
    }
    const Sweep& s = sweep();
    if (s.oracle_cases == 0) {
        GTEST_SKIP() << "oracle runner unavailable; hardware column not run";
    }
    Limits lim;
    if (std::getenv("VITREO_ISA_MATRIX_WRITE_BASELINE") != nullptr) {
        GTEST_SKIP() << "baseline rewritten by the parity test";
    }
    if (!read_limits(&lim)) GTEST_SKIP() << "no baseline yet";
    EXPECT_LE(s.hardware_failures, lim.hardware)
        << "divergences from real x86 rose from " << lim.hardware << " to "
        << s.hardware_failures << ".\nExamples:" << join(s.hardware_examples);
}

/// The JIT's XMM register cache keeps XMMs in V16-V30 across instructions and
/// trusts JitCompiler::xmm_cache_written_mask to name every XMM an
/// instruction that is not cache-aware can write: one it misses is left
/// stale in its cache register and read later. The interpreter is the
/// semantic reference for what an instruction writes, so run every
/// executable 64-bit form through it and require each XMM that changed to be
/// in the mask. (Only a value that CHANGES can go stale, so this is the exact
/// criterion, over this sweep's seeds.)
TEST(IsaMatrixExec, LegacyFormsWriteOnlyTheXmmsTheCacheDrops) {
    namespace oc = vitreo::oracle::contract;
    using vitreo::cpu::jit::JitCompiler;
    std::vector<std::string> bad;
    unsigned checked = 0;
    unsigned writers = 0;
    for (const FormCase& fc : vitreo::isa_matrix::enumerate_forms(64)) {
        if (vitreo::isa_matrix::classify(fc) != SkipReason::None) continue;
        const unsigned may = JitCompiler::xmm_cache_written_mask(fc.decoded);
        if (may == 0xFFFFu) continue;
        bool wrote = false;
        for (unsigned seed = 0; seed < kSeeds; ++seed) {
            const vitreo::oracle::State in =
                vitreo::isa_matrix::seed_state(fc, seed);
            const vitreo::oracle::State out =
                oc::run_on_interpreter_64(in, fc.bytes);
            if (out.status != 0) continue;
            ++checked;
            for (unsigned r = 0; r < 16; ++r) {
                if (in.xmm[r] == out.xmm[r]) continue;
                wrote = true;
                if ((may & (1u << r)) == 0 && bad.size() < 40) {
                    bad.push_back(fc.text() + " wrote xmm" +
                                  std::to_string(r));
                }
            }
        }
        writers += wrote ? 1u : 0u;
    }
    std::string list;
    for (const std::string& b : bad) list += "\n  " + b;
    EXPECT_TRUE(bad.empty())
        << "an instruction wrote an XMM that xmm_cache_written_mask does not "
           "name; the XMM cache would serve a stale value:" << list;
    EXPECT_GT(checked, 1000u) << "the write-set audit ran almost nothing";
    EXPECT_GT(writers, 200u) << "almost no form wrote an XMM; the audit is "
                                "not seeing vector forms";
}
