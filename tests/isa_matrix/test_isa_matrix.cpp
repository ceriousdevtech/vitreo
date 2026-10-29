// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_isa_matrix.cpp
/// @brief The ISA matrix gates.
///
/// Two gates, separate because they fail for different reasons:
///
///   * **Enumerator health** — the form enumerator must keep finding forms.
///     If a decoder change made the enumerator's candidates stop decoding, the
///     coverage ratchet below would read 100% of nothing and pass forever.
///   * **Coverage ratchet** — per (mnemonic, bitness), the number of natively
///     lowered forms may rise but never fall, against the committed baseline.
///
/// The ratchet is the gate that catches the failure mode no behavioural test
/// can see: a lowering that stops being *reached*. The interpreter delegate
/// returns identical answers, so a dispatch-chain edit that unwires a whole
/// family passes every parity and oracle test in the tree. That is how x87 sat
/// at 0% on 64-bit blocks unnoticed until someone measured it.

#include "isa_form_enum.hpp"
#include "isa_matrix_probe.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

using vitreo::cpu::jit::LowerKind;
using vitreo::isa_matrix::FormCase;

/// Native-form count per "MNEMONIC/bitness".
using Baseline = std::map<std::string, unsigned>;

struct Measured {
    Baseline native;
    /// Emitted ARM64 words per "MNEMONIC/bitness", summed over every natively
    /// lowered form of that mnemonic.
    ///
    /// Emitted ARM64 words, summed over every natively lowered form. It is
    /// noise-free: it is what the compiler emitted, not how long anything ran,
    /// so it is meaningful on a loaded machine and in CI. It exists because a
    /// correctness fix quietly added ~49 ARM instructions to
    /// `FLD m32` -- a hot instruction in 32-bit games -- and nothing failed.
    Baseline words;
    std::size_t forms = 0;
    unsigned total_native = 0;
    unsigned total_helper = 0;
    unsigned total_delegate = 0;
    unsigned total_declined = 0;
};

Measured measure() {
    Measured m;
    const std::vector<FormCase> forms =
        vitreo::isa_matrix::enumerate_all_forms();
    m.forms = forms.size();
    for (const FormCase& fc : forms) {
        const char* n = vitreo::cpu::opcode_name(fc.opcode);
        std::ostringstream key;
        key << (n ? n : "(unnamed)") << '/' << fc.mode;
        // Ensure every mnemonic appears, so a form that regresses from native
        // to delegate shows as a decrease rather than a missing key.
        m.native.try_emplace(key.str(), 0u);

        m.words.try_emplace(key.str(), 0u);
        const auto p = vitreo::isa_matrix::probe_form(fc);
        // VITREO_ISA_MATRIX_DUMP=<substring>: every form of the matching
        // mnemonics with its lowering kind, to name the forms behind a
        // ratchet failure (which reports only per-mnemonic counts).
        static const char* dump = std::getenv("VITREO_ISA_MATRIX_DUMP");
        if (dump != nullptr && key.str().find(dump) != std::string::npos) {
            const char* kind = !p.compiled ? "declined"
                : p.kind == LowerKind::Native ? "native"
                : p.kind == LowerKind::Helper ? "helper" : "delegate";
            std::printf("[isa-matrix] %-40s %-8s %s\n", key.str().c_str(),
                        kind, fc.text().c_str());
        }
        if (!p.compiled) { ++m.total_declined; continue; }
        switch (p.kind) {
        case LowerKind::Native:
            ++m.native[key.str()];
            m.words[key.str()] += p.arm_words;
            ++m.total_native;
            break;
        case LowerKind::Helper:   ++m.total_helper; break;
        case LowerKind::Delegate: ++m.total_delegate; break;
        }
    }
    return m;
}

/// Minimal reader for the baseline this test itself writes. Deliberately not a
/// general JSON parser: the file is machine-written by `write_baseline` below
/// and its shape is one `"KEY": N,` pair per line.
Baseline read_baseline(const std::string& path, bool* found) {
    Baseline b;
    std::ifstream in(path);
    *found = in.good();
    if (!in.good()) return b;
    std::string line;
    while (std::getline(in, line)) {
        const auto q1 = line.find('"');
        if (q1 == std::string::npos) continue;
        const auto q2 = line.find('"', q1 + 1);
        if (q2 == std::string::npos) continue;
        const auto colon = line.find(':', q2);
        if (colon == std::string::npos) continue;
        b[line.substr(q1 + 1, q2 - q1 - 1)] =
            static_cast<unsigned>(std::strtoul(line.c_str() + colon + 1,
                                               nullptr, 10));
    }
    return b;
}

void write_baseline(const std::string& path, const Measured& m) {
    std::ofstream out(path);
    ASSERT_TRUE(out.good()) << "cannot write " << path;
    out << "{\n";
    std::size_t i = 0;
    for (const auto& [key, n] : m.native) {
        out << "  \"" << key << "\": " << n
            << (++i == m.native.size() ? "" : ",") << "\n";
    }
    out << "}\n";
}

const char* words_path() {
#ifdef VITREO_ISA_MATRIX_WORDS_BASELINE
    return VITREO_ISA_MATRIX_WORDS_BASELINE;
#else
    return "";
#endif
}

const char* baseline_path() {
#ifdef VITREO_ISA_MATRIX_BASELINE
    return VITREO_ISA_MATRIX_BASELINE;
#else
    return "";
#endif
}

void write_words(const std::string& path, const Measured& m) {
    std::ofstream out(path);
    ASSERT_TRUE(out.good()) << "cannot write " << path;
    out << "{\n";
    std::size_t i = 0;
    for (const auto& [key, n] : m.words) {
        out << "  \"" << key << "\": " << n
            << (++i == m.words.size() ? "" : ",") << "\n";
    }
    out << "}\n";
}

const Measured& measured() {
    // One enumeration + probe pass shared by both tests: the pass compiles
    // every form in the ISA and is the expensive part of this binary.
    static const Measured m = measure();
    return m;
}

}  // namespace

/// The enumerator must keep producing a large, stable form set. A ratchet over
/// an empty set is a gate that cannot fail.
TEST(IsaMatrix, EnumeratorFindsTheInstructionSet) {
    const Measured& m = measured();
    EXPECT_GT(m.forms, 2000u)
        << "the decoder-inversion enumerator collapsed: it found only "
        << m.forms << " forms. A decoder or enumerator change has made the "
           "candidate encodings stop decoding, which would silently disarm "
           "the coverage ratchet.";
    EXPECT_GT(m.total_native, 0u);

    std::printf("[isa-matrix] %zu forms: %u native, %u helper, %u delegate, "
                "%u declined\n",
                m.forms, m.total_native, m.total_helper, m.total_delegate,
                m.total_declined);
}

/// Native coverage may rise, never fall.
///
/// Re-record deliberately, in the same commit as the change that justifies it:
///     VITREO_ISA_MATRIX_WRITE_BASELINE=1 ./build/tests/vitreo_isa_matrix_tests
TEST(IsaMatrix, NativeCoverageRatchet) {
    const Measured& m = measured();
    const std::string path = baseline_path();
    ASSERT_FALSE(path.empty()) << "baseline path not configured";

    if (std::getenv("VITREO_ISA_MATRIX_WRITE_BASELINE") != nullptr) {
        write_baseline(path, m);
        GTEST_SKIP() << "baseline rewritten: " << path;
    }

    bool found = false;
    const Baseline base = read_baseline(path, &found);
    if (!found) {
        write_baseline(path, m);
        GTEST_SKIP() << "no baseline yet; wrote one at " << path
                     << ". Commit it — the ratchet is inert until you do.";
    }

    std::vector<std::string> regressions;
    for (const auto& [key, want] : base) {
        const auto it = m.native.find(key);
        const unsigned got = (it == m.native.end()) ? 0u : it->second;
        if (got < want) {
            regressions.push_back(key + ": " + std::to_string(got) +
                                  " native forms, baseline " +
                                  std::to_string(want));
        }
    }

    std::string detail;
    for (const std::string& r : regressions) detail += "\n  " + r;
    EXPECT_TRUE(regressions.empty())
        << regressions.size() << " mnemonic(s) lost native JIT coverage."
        << detail
        << "\n\nA lowering that stops being REACHED passes every behavioural "
           "test, because the interpreter delegate returns the same answer. "
           "If this drop is intended, re-record with "
           "VITREO_ISA_MATRIX_WRITE_BASELINE=1 in the same commit.";
}

/// Emitted code size may fall, never rise.
///
/// Per-form emitted-word budgets.
/// A lowering that grows is not automatically wrong -- the x87 push guard
/// added ~49 words to FLD m32 and was a correctness fix -- but it must be a
/// decision somebody made, not a side effect nobody measured. Re-record in the
/// same commit that justifies the growth, and say why:
///     VITREO_ISA_MATRIX_WRITE_BASELINE=1 ./build/tests/vitreo_isa_matrix_tests
TEST(IsaMatrix, EmittedCodeSizeRatchet) {
    const Measured& m = measured();
    const std::string path = words_path();
    ASSERT_FALSE(path.empty()) << "words baseline path not configured";

    if (std::getenv("VITREO_ISA_MATRIX_WRITE_BASELINE") != nullptr) {
        write_words(path, m);
        GTEST_SKIP() << "words baseline rewritten: " << path;
    }
    bool found = false;
    const Baseline base = read_baseline(path, &found);
    if (!found) {
        write_words(path, m);
        GTEST_SKIP() << "no words baseline yet; wrote one at " << path
                     << ". Commit it — the ratchet is inert until you do.";
    }

    std::vector<std::string> grew;
    unsigned long long total_now = 0, total_was = 0;
    for (const auto& [key, was] : base) {
        const auto it = m.words.find(key);
        const unsigned now = (it == m.words.end()) ? 0u : it->second;
        total_now += now;
        total_was += was;
        if (now > was) {
            grew.push_back(key + ": " + std::to_string(now) + " words, was " +
                           std::to_string(was));
        }
    }
    std::string detail;
    for (std::size_t i = 0; i < grew.size() && i < 25; ++i) {
        detail += "\n  " + grew[i];
    }
    std::printf("[isa-matrix] emitted words: %llu (baseline %llu)\n",
                total_now, total_was);
    EXPECT_TRUE(grew.empty())
        << grew.size() << " mnemonic(s) emit more ARM64 than the baseline."
        << detail
        << "\n\nIf the growth is intended, re-record with "
           "VITREO_ISA_MATRIX_WRITE_BASELINE=1 in the same commit and say why.";
}
