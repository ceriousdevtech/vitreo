// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12_pso_replay.cpp
/// @brief Pipeline-creation benchmark: replays VITREO_D3D12_DUMP_PSOS
///        directories through the real D3D12 frontend and Metal backend
///        (d3d12_pso_replay_core.hpp) and reports where the time went.
///
///   vitreo_d3d12_pso_replay [--threads N] [--no-draw] [--verbose] <dir>...
///
/// One configuration per process: the frontend's and Metal's in-process
/// memos would make a second pass in the same process warm. Cold numbers
/// need Metal's own per-user shader cache defeated as well: run with
/// VITREO_METAL_COMPILE_SALT=<fresh string> (every Metal library compile
/// then sees source it has never seen) and with the persistent translation
/// cache pointed at an empty directory (VITREO_SHADER_CACHE_DIR).

#include "d3d12_pso_replay_core.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi::pso_replay;

namespace {

double ms(u64 ns) { return static_cast<double>(ns) / 1e6; }

const char* const kPhaseNames[cp::kPhaseCount] = {
    "dxil_lower", "dxbc_to_spirv", "spirv_to_msl", "metal_library", "metal_render_state",
    "metal_compute_state"};

void print_phases(const char* label, const cp::Snapshot& a, const cp::Snapshot& b, u64 wall_ns) {
    std::printf("%s phases (sum ms / count / max at once / sum/wall):\n", label);
    for (int p = 0; p < cp::kPhaseCount; ++p) {
        const u64 n = b.count[p] - a.count[p];
        const u64 ns = b.ns[p] - a.ns[p];
        std::printf("  %-20s %10.1f ms %7llu  max %3llu  %5.2f\n", kPhaseNames[p], ms(ns),
                    static_cast<unsigned long long>(n),
                    static_cast<unsigned long long>(b.max_in_flight[p]),
                    wall_ns != 0 ? static_cast<double>(ns) / static_cast<double>(wall_ns) : 0.0);
    }
    std::printf("  cache: msl hit %llu miss %llu, dxil hit %llu miss %llu\n",
                static_cast<unsigned long long>(b.cache[cp::MslHit] - a.cache[cp::MslHit]),
                static_cast<unsigned long long>(b.cache[cp::MslMiss] - a.cache[cp::MslMiss]),
                static_cast<unsigned long long>(b.cache[cp::DxilHit] -
                                                a.cache[cp::DxilHit]),
                static_cast<unsigned long long>(b.cache[cp::DxilMiss] -
                                                a.cache[cp::DxilMiss]));
}

}  // namespace

int main(int argc, char** argv) {
    unsigned threads = 1;
    bool draw = true;
    bool verbose = false;
    std::vector<std::string> dirs;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--threads" && i + 1 < argc) {
            threads = static_cast<unsigned>(std::atoi(argv[++i]));
        } else if (arg == "--no-draw") {
            draw = false;
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg.starts_with("--")) {
            std::fprintf(stderr, "unknown option %s\n", arg.c_str());
            return 2;
        } else {
            dirs.push_back(arg);
        }
    }
    if (dirs.empty()) {
        std::fprintf(stderr,
                     "usage: %s [--threads N] [--no-draw] [--verbose] <dump dir>...\n",
                     argv[0]);
        return 2;
    }
    std::vector<Dump> dumps;
    for (const std::string& dir : dirs) {
        std::string error;
        auto loaded = load_dumps(dir, error);
        if (!error.empty()) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        for (auto& d : loaded) dumps.push_back(std::move(d));
    }
    std::string error;
    Replayer replayer(error);
    if (!error.empty() || !replayer.prepare(dumps, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    const RunResult run = replayer.run(threads, draw);
    usize created = 0, failed = 0, skipped = 0;
    std::vector<u64> times;
    for (const PsoResult& r : run.pipelines) {
        if (r.skipped) {
            ++skipped;
            if (verbose) std::printf("skip %s: %s\n", r.name.c_str(), r.skip_reason.c_str());
            continue;
        }
        if ((r.hr & 0x80000000u) != 0) {
            ++failed;
        } else {
            ++created;
        }
        times.push_back(r.create_ns);
        if (verbose) {
            std::printf("%s hr=0x%08x create %.1f ms draw %.1f ms", r.name.c_str(), r.hr,
                        ms(r.create_ns), ms(r.draw_ns));
            for (int p = 0; p < cp::kPhaseCount; ++p) {
                const u64 ns = r.phases_after.ns[p] - r.phases_before.ns[p];
                if (ns != 0) std::printf(" %s %.1f", kPhaseNames[p], ms(ns));
            }
            std::printf("\n");
        }
    }
    std::sort(times.begin(), times.end());
    const auto pct = [&](double q) {
        return times.empty() ? 0.0 : ms(times[std::min(times.size() - 1,
                                                       static_cast<usize>(q * times.size()))]);
    };
    const char* salt = std::getenv("VITREO_METAL_COMPILE_SALT");
    std::printf("pipelines: %zu created, %zu failed, %zu skipped; threads %u; salt %s\n",
                created, failed, skipped, threads, salt != nullptr ? salt : "(none)");
    std::printf("create: wall %.1f ms; per pipeline median %.1f p90 %.1f max %.1f ms\n",
                ms(run.create_wall_ns), pct(0.5), pct(0.9), pct(1.0));
    print_phases("create", run.before, run.after_create, run.create_wall_ns);
    if (draw) {
        std::printf("first draws: wall %.1f ms\n", ms(run.draw_wall_ns));
        print_phases("draw", run.after_create, run.after_draw, run.draw_wall_ns);
    }
    std::printf("total: %.1f ms\n", ms(run.create_wall_ns + run.draw_wall_ns));
    {
        const cp::Snapshot now = cp::snapshot();
        std::printf("samplers: %llu created (%llu MINIMUM/MAXIMUM), %llu static (%llu "
                    "MINIMUM/MAXIMUM)\n",
                    static_cast<unsigned long long>(now.samplers[cp::SamplerCreated]),
                    static_cast<unsigned long long>(now.samplers[cp::SamplerMinMax]),
                    static_cast<unsigned long long>(now.samplers[cp::StaticSamplerCreated]),
                    static_cast<unsigned long long>(now.samplers[cp::StaticSamplerMinMax]));
    }
    return failed != 0 ? 3 : 0;
}
