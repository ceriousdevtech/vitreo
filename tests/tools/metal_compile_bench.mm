// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_compile_bench.mm
/// @brief Metal library compile timing over MSL files (a VITREO_DUMP_MSL_DIR
///        dump, or any .metal files): how long newLibraryWithSource takes per
///        file and in total on N threads, under chosen compile options.
///
///   vitreo_metal_compile_bench [--threads N] [--async]
///       [--math safe|relaxed|fast] [--opt default|size] [--invariance 0|1]
///       [--strip] [--salt TEXT] [--pipeline] <file or dir>...
///
/// --salt renames main0 (as VITREO_METAL_COMPILE_SALT does) so the compiler
/// service's per-user cache cannot answer; --pipeline also builds a render
/// pipeline (vertex + a pass-through, or a fragment with a trivial vertex)
/// or compute pipeline from each function, as a first draw would;
/// --async uses the completion-handler variant with N in flight.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <condition_variable>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

double now_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string strip_indent(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool line_start = true;
    for (char c : text) {
        if (line_start && (c == ' ' || c == '\t')) continue;
        line_start = c == '\n';
        out += c;
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        unsigned threads = 1;
        bool async = false;
        int math = 1;  // relaxed
        bool size_opt = false;
        int invariance = -1;  // vertex functions only
        bool strip = false;
        bool pipeline = false;
        std::string salt;
        std::vector<std::filesystem::path> files;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
            if (a == "--threads") threads = static_cast<unsigned>(std::atoi(next().c_str()));
            else if (a == "--async") async = true;
            else if (a == "--math") {
                const std::string m = next();
                math = m == "safe" ? 2 : m == "fast" ? 0 : 1;
            } else if (a == "--opt") size_opt = next() == "size";
            else if (a == "--invariance") invariance = std::atoi(next().c_str());
            else if (a == "--strip") strip = true;
            else if (a == "--pipeline") pipeline = true;
            else if (a == "--salt") salt = next();
            else if (std::filesystem::is_directory(a)) {
                for (const auto& e : std::filesystem::recursive_directory_iterator(a)) {
                    const auto ext = e.path().extension();
                    if (ext == ".metal" || ext == ".msl") files.push_back(e.path());
                }
            } else {
                files.push_back(a);
            }
        }
        std::sort(files.begin(), files.end());
        struct Job {
            std::string name;
            std::string source;
            bool vertex{false};
            bool fragment{false};
            double ms{0};
            double pipeline_ms{0};
            bool ok{false};
        };
        std::vector<Job> jobs;
        for (const auto& f : files) {
            std::ifstream in(f, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            Job j;
            j.name = f.filename().string();
            j.source = strip ? strip_indent(ss.str()) : ss.str();
            j.vertex = j.source.find("vertex ") != std::string::npos &&
                       j.source.find("\nvertex ") != std::string::npos;
            j.fragment = j.source.find("\nfragment ") != std::string::npos;
            if (!salt.empty()) j.source = "#define main0 main0_s" + salt + "\n" + j.source;
            jobs.push_back(std::move(j));
        }
        // MTLCreateSystemDefaultDevice is one object per GPU per process.
        std::vector<id<MTLDevice>> devs{MTLCreateSystemDefaultDevice()};
        const NSString* entry =
            salt.empty() ? @"main0" : [NSString stringWithFormat:@"main0_s%s", salt.c_str()];
        const auto options = [&](bool vertex) {
            MTLCompileOptions* o = [[MTLCompileOptions alloc] init];
            o.mathMode = math == 2 ? MTLMathModeSafe : math == 0 ? MTLMathModeFast : MTLMathModeRelaxed;
            if (size_opt) o.optimizationLevel = MTLLibraryOptimizationLevelSize;
            o.preserveInvariance = invariance < 0 ? (vertex ? YES : NO) : (invariance ? YES : NO);
            return o;
        };
        id<MTLLibrary> helpers = nil;
        {
            NSError* e = nil;
            helpers = [devs[0] newLibraryWithSource:
                                   @"#include <metal_stdlib>\nusing namespace metal;\n"
                                    "vertex float4 vitreo_bench_vs(uint v [[vertex_id]]) { return float4(0); }\n"
                                    "fragment void vitreo_bench_fs() {}\n"
                                       options:nil error:&e];
        }
        const auto make_pipeline = [&](id<MTLDevice> dev, id<MTLFunction> fn, Job& j) {
            NSError* e = nil;
            const double t0 = now_ms();
            if (fn.functionType == MTLFunctionTypeKernel) {
                (void)[dev newComputePipelineStateWithFunction:fn error:&e];
            } else {
                MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor alloc] init];
                if (fn.functionType == MTLFunctionTypeVertex) {
                    d.vertexFunction = fn;
                    d.rasterizationEnabled = NO;
                } else {
                    d.vertexFunction = [helpers newFunctionWithName:@"vitreo_bench_vs"];
                    d.fragmentFunction = fn;
                    for (int k = 0; k < 8; ++k) d.colorAttachments[k].pixelFormat = MTLPixelFormatRGBA16Float;
                    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
                    d.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
                }
                (void)[dev newRenderPipelineStateWithDescriptor:d error:&e];
            }
            j.pipeline_ms = now_ms() - t0;
        };
        std::atomic<size_t> next{0};
        const double wall0 = now_ms();
        if (async) {
            struct Gate {
                std::mutex m;
                std::condition_variable cv;
                size_t in_flight = 0, done = 0;
            };
            Gate gate;
            Gate* g = &gate;
            for (size_t i = 0; i < jobs.size(); ++i) {
                {
                    std::unique_lock lock(g->m);
                    g->cv.wait(lock, [&] { return g->in_flight < threads; });
                    ++g->in_flight;
                }
                Job* j = &jobs[i];
                id<MTLDevice> dev = devs[i % devs.size()];
                const double t0 = now_ms();
                [dev newLibraryWithSource:@(j->source.c_str())
                                  options:options(j->vertex)
                        completionHandler:^(id<MTLLibrary> lib, NSError*) {
                          j->ms = now_ms() - t0;
                          j->ok = lib != nil;
                          std::lock_guard lock(g->m);
                          --g->in_flight;
                          ++g->done;
                          g->cv.notify_all();
                        }];
            }
            std::unique_lock lock(g->m);
            g->cv.wait(lock, [&] { return g->done == jobs.size(); });
        } else {
            const auto worker = [&](unsigned slot) {
                @autoreleasepool {
                    for (;;) {
                        const size_t i = next.fetch_add(1);
                        if (i >= jobs.size()) return;
                        Job& j = jobs[i];
                        id<MTLDevice> dev = devs[(slot + i) % devs.size()];
                        NSError* e = nil;
                        const double t0 = now_ms();
                        id<MTLLibrary> lib = [dev newLibraryWithSource:@(j.source.c_str())
                                                               options:options(j.vertex)
                                                                 error:&e];
                        j.ms = now_ms() - t0;
                        j.ok = lib != nil;
                        if (!j.ok) {
                            std::fprintf(stderr, "%s: %s\n", j.name.c_str(),
                                         e != nil ? e.localizedDescription.UTF8String : "?");
                        }
                        if (lib != nil && pipeline) {
                            id<MTLFunction> fn = [lib newFunctionWithName:(NSString*)entry];
                            if (fn != nil) make_pipeline(dev, fn, j);
                        }
                    }
                }
            };
            std::vector<std::thread> pool;
            for (unsigned t = 0; t < std::max(1u, threads); ++t) pool.emplace_back(worker, t);
            for (auto& t : pool) t.join();
        }
        const double wall = now_ms() - wall0;
        double sum = 0, psum = 0;
        size_t failed = 0, bytes = 0;
        for (const Job& j : jobs) {
            sum += j.ms;
            psum += j.pipeline_ms;
            failed += j.ok ? 0 : 1;
            bytes += j.source.size();
        }
        std::vector<double> t;
        for (const Job& j : jobs) t.push_back(j.ms);
        std::sort(t.begin(), t.end());
        if (std::getenv("BENCH_VERBOSE") != nullptr) {
            for (const Job& j : jobs) {
                std::printf("%-40s %8.1f ms  pipeline %8.1f ms  %zu bytes\n", j.name.c_str(), j.ms,
                            j.pipeline_ms, j.source.size());
            }
        }
        std::printf("files %zu (%zu failed, %.1f MB) threads %u devices %u%s: wall %.0f ms, "
                    "library sum %.0f ms (median %.1f, max %.1f), concurrency %.2f, pipeline sum %.0f ms\n",
                    jobs.size(), failed, bytes / 1e6, threads, 1u, async ? " async" : "", wall,
                    sum, t.empty() ? 0 : t[t.size() / 2], t.empty() ? 0 : t.back(),
                    wall > 0 ? sum / wall : 0, psum);
    }
    return 0;
}
