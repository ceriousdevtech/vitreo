// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file isa_matrix_report.cpp
/// @brief `vitreo-isa-matrix` — the ISA scoreboard.
///
/// Enumerates every instruction form the decoder accepts, compiles each one,
/// and reports how the JIT lowered it: native ARM64, a C++ semantic helper, or
/// the interpreter delegate. This replaces `tools/isa_audit.py`'s source scan
/// as the coverage measure. A scan
/// of `case Opcode::` arms systematically overstates coverage.
///
///     vitreo-isa-matrix                 summary per bitness
///     vitreo-isa-matrix --gaps          every non-native form
///     vitreo-isa-matrix --json FILE     write the machine-readable matrix
///     vitreo-isa-matrix --opcode NAME   drill into one mnemonic

#include "isa_form_enum.hpp"
#include "isa_matrix_probe.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

using vitreo::cpu::jit::LowerKind;
using vitreo::isa_matrix::FormCase;
using vitreo::isa_matrix::ProbeResult;

struct Row {
    const FormCase* form;
    ProbeResult probe;
};

struct Counts {
    unsigned native = 0;
    unsigned helper = 0;
    unsigned delegate = 0;
    unsigned declined = 0;
    [[nodiscard]] unsigned total() const {
        return native + helper + delegate + declined;
    }
};

void tally(Counts& c, const Row& r) {
    if (!r.probe.compiled) { ++c.declined; return; }
    switch (r.probe.kind) {
    case LowerKind::Native:   ++c.native; break;
    case LowerKind::Helper:   ++c.helper; break;
    case LowerKind::Delegate: ++c.delegate; break;
    }
}

double pct(unsigned n, unsigned d) {
    return d == 0 ? 0.0 : 100.0 * static_cast<double>(n) / d;
}

const char* kind_name(const Row& r) {
    if (!r.probe.compiled) return "declined";
    return vitreo::cpu::jit::lower_kind_name(r.probe.kind);
}

void print_summary(const std::vector<Row>& rows) {
    Counts per_mode[2];
    for (const Row& r : rows) tally(per_mode[r.form->mode == 64 ? 1 : 0], r);

    std::printf("%-8s %8s %9s %9s %10s %10s\n",
                "BITNESS", "FORMS", "NATIVE", "HELPER", "DELEGATE", "DECLINED");
    for (int m = 0; m < 2; ++m) {
        const Counts& c = per_mode[m];
        if (c.total() == 0) continue;
        std::printf("%-8s %8u %8.1f%% %8.1f%% %9.1f%% %10u\n",
                    m == 0 ? "x86" : "x64", c.total(),
                    pct(c.native, c.total()), pct(c.helper, c.total()),
                    pct(c.delegate, c.total()), c.declined);
    }

    Counts all;
    for (const Row& r : rows) tally(all, r);
    std::printf("%-8s %8u %8.1f%% %8.1f%% %9.1f%% %10u\n",
                "TOTAL", all.total(), pct(all.native, all.total()),
                pct(all.helper, all.total()), pct(all.delegate, all.total()),
                all.declined);
}

/// Rank the mnemonics with the most non-native forms: the worklist.
void print_worklist(const std::vector<Row>& rows, unsigned limit) {
    std::map<std::string, Counts> by_op;
    for (const Row& r : rows) {
        const char* n = vitreo::cpu::opcode_name(r.form->opcode);
        tally(by_op[n ? n : "(unnamed)"], r);
    }
    std::vector<std::pair<std::string, Counts>> ranked(by_op.begin(),
                                                       by_op.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        const unsigned an = a.second.helper + a.second.delegate +
                            a.second.declined;
        const unsigned bn = b.second.helper + b.second.delegate +
                            b.second.declined;
        if (an != bn) return an > bn;
        return a.first < b.first;
    });

    std::printf("\nTop mnemonics by non-native forms:\n");
    std::printf("  %-28s %7s %7s %9s %9s\n",
                "MNEMONIC", "NATIVE", "HELPER", "DELEGATE", "DECLINED");
    unsigned shown = 0;
    for (const auto& [name, c] : ranked) {
        if (c.helper + c.delegate + c.declined == 0) break;
        if (shown++ >= limit) break;
        std::printf("  %-28s %7u %7u %9u %9u\n", name.c_str(), c.native,
                    c.helper, c.delegate, c.declined);
    }
}

void print_gaps(const std::vector<Row>& rows, const char* only_opcode) {
    for (const Row& r : rows) {
        const char* n = vitreo::cpu::opcode_name(r.form->opcode);
        const std::string name = n ? n : "(unnamed)";
        if (only_opcode != nullptr &&
            name.find(only_opcode) == std::string::npos) {
            continue;
        }
        const bool native = r.probe.compiled &&
                            r.probe.kind == LowerKind::Native;
        if (native && only_opcode == nullptr) continue;
        std::printf("  %-10s %s\n", kind_name(r), r.form->text().c_str());
    }
}

void write_json(const std::vector<Row>& rows, const char* path) {
    std::FILE* f = std::fopen(path, "w");
    if (f == nullptr) {
        std::fprintf(stderr, "isa-matrix: cannot write %s\n", path);
        return;
    }
    std::fprintf(f, "{\n  \"forms\": [\n");
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        const char* n = vitreo::cpu::opcode_name(r.form->opcode);
        char tags[96];
        vitreo::cpu::jit::format_form_key(r.form->form, tags, sizeof(tags));
        std::fprintf(f,
            "    {\"opcode\": \"%s\", \"form\": \"%s\", \"form_bits\": %u, "
            "\"mode\": %u, \"lowering\": \"%s\", \"arm_words\": %u}%s\n",
            n ? n : "(unnamed)", tags, r.form->form.bits, r.form->mode,
            kind_name(r), r.probe.arm_words,
            i + 1 == rows.size() ? "" : ",");
    }
    std::fprintf(f, "  ]\n}\n");
    std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
    bool gaps = false;
    const char* json_path = nullptr;
    const char* only_opcode = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--gaps") == 0) {
            gaps = true;
        } else if (std::strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
            json_path = argv[++i];
        } else if (std::strcmp(argv[i], "--opcode") == 0 && i + 1 < argc) {
            only_opcode = argv[++i];
        } else {
            std::fprintf(stderr,
                "usage: %s [--gaps] [--opcode NAME] [--json FILE]\n", argv[0]);
            return 2;
        }
    }

    const std::vector<FormCase> forms =
        vitreo::isa_matrix::enumerate_all_forms();
    std::vector<Row> rows;
    rows.reserve(forms.size());
    for (const FormCase& fc : forms) {
        rows.push_back(Row{&fc, vitreo::isa_matrix::probe_form(fc)});
    }

    print_summary(rows);
    print_worklist(rows, 25);
    if (gaps || only_opcode != nullptr) {
        std::printf("\nForms:\n");
        print_gaps(rows, only_opcode);
    }
    if (json_path != nullptr) write_json(rows, json_path);
    return 0;
}
