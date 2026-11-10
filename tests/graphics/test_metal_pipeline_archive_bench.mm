// SPDX-License-Identifier: LGPL-2.1-only
//
// WS4 upper-bound probe: does a persistent MTLBinaryArchive beat the pipeline
// cache Metal already keeps for every app?
//
// Contract (MTLBinaryArchive.h, MTLDevice.h, macOS 26.3 SDK): Metal "maintains
// a separate cache of pipeline states on behalf of each app", which already
// accelerates a second process's library and pipeline creation. An archive
// attached through `binaryArchives` and looked up with
// MTLPipelineOptionFailOnBinaryArchiveMiss serves the backend (AIR -> GPU)
// compile. No public API persists the front end (newLibraryWithSource).
//
// This file measures the upper bound with raw Metal only (no Vitreo API), so
// the number is what any archive implementation could at best deliver. The
// corpus is generated in-tree: no shipped or Windows-derived shader bytes.
//
// The tests are DISABLED_ and run explicitly:
//   vitreo_metal_pipeline_archive_tests --gtest_also_run_disabled_tests \
//       --gtest_filter=MetalPipelineArchiveBench.DISABLED_UpperBound

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <gtest/gtest.h>

#include <mach-o/dyld.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace ws4bench {

constexpr int kCorpusPairs = 16;

namespace {

double now_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Helper functions with distinct constants so each source is several KB of
// real work for the front end and the backend, and the nonce changes the AIR.
std::string helpers(unsigned nonce, int i, const char* prefix) {
    std::string out;
    char buf[1024];
    for (int h = 0; h < 10; ++h) {
        std::snprintf(
            buf, sizeof buf,
            "static float4 %s_h%d_%d(float4 v, uint salt) {\n"
            "  float4 a = v * %d.25f + float4(float(salt ^ %uu));\n"
            "  for (int r = 0; r < %d; ++r) {\n"
            "    a = fma(a, float4(1.0001f), sin(a.yzwx) * cos(a * %d.5f));\n"
            "    if (a.x > 1e5f) a = sqrt(abs(a));\n"
            "    a.xy = a.xy * float2x2(float2(cos(a.z), -sin(a.z)),"
            " float2(sin(a.z), cos(a.z)));\n"
            "  }\n"
            "  return a + float4(tanh(a.x), exp(a.y * 1e-3f),"
            " log(abs(a.z) + 1.0f), pow(abs(a.w) + 1.0f, 0.%d5f));\n"
            "}\n",
            prefix, i, h, h + 1, nonce + static_cast<unsigned>(h), 3 + h, h + i + 1,
            h + 1);
        out += buf;
    }
    return out;
}

}  // namespace

std::string corpus_kernel(unsigned nonce, int i) {
    std::string src =
        "#include <metal_stdlib>\nusing namespace metal;\n" + helpers(nonce, i, "k");
    char buf[2048];
    std::snprintf(
        buf, sizeof buf,
        "kernel void k%d(device float4* o [[buffer(0)]], constant uint& n [[buffer(1)]],\n"
        "                uint gid [[thread_position_in_grid]]) {\n"
        "  float4 v = float4(float(gid ^ %uu));\n"
        "  for (uint j = 0; j < n; ++j) {\n"
        "    v = k_h%d_0(v, j); v = k_h%d_1(v, j); v = k_h%d_2(v, j); v = k_h%d_3(v, j);\n"
        "    v = k_h%d_4(v, j); v = k_h%d_5(v, j); v = k_h%d_6(v, j); v = k_h%d_7(v, j);\n"
        "    v = k_h%d_8(v, j); v = k_h%d_9(v, j);\n"
        "  }\n"
        "  o[gid] = v;\n"
        "}\n",
        i, nonce, i, i, i, i, i, i, i, i, i, i);
    return src + buf;
}

std::string corpus_render(unsigned nonce, int i) {
    std::string src =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct VOut { float4 pos [[position]]; float4 c [[user(locn0)]]; };\n" +
        helpers(nonce, i, "r");
    char buf[2048];
    std::snprintf(
        buf, sizeof buf,
        "vertex VOut vs%d(uint vid [[vertex_id]], constant float4* p [[buffer(0)]]) {\n"
        "  VOut o; float4 q = p[vid];\n"
        "  q = r_h%d_0(q, vid); q = r_h%d_1(q, vid); q = r_h%d_2(q, vid);"
        " q = r_h%d_3(q, vid); q = r_h%d_4(q, vid);\n"
        "  o.pos = q; o.c = float4(float(vid ^ %uu)); return o;\n"
        "}\n"
        "fragment float4 fs%d(VOut in [[stage_in]], texture2d<float> t [[texture(0)]],\n"
        "                     sampler s [[sampler(0)]]) {\n"
        "  float4 c = in.c * t.sample(s, in.c.xy);\n"
        "  c = r_h%d_5(c, 1u); c = r_h%d_6(c, 2u); c = r_h%d_7(c, 3u);"
        " c = r_h%d_8(c, 4u); c = r_h%d_9(c, 5u);\n"
        "  return c;\n"
        "}\n",
        i, i, i, i, i, i, nonce, i, i, i, i, i, i);
    return src + buf;
}

struct RunResult {
    bool ok{false};
    double lib_ms{0}, pso_ms{0}, load_ms{0}, add_ms{0}, ser_ms{0};
    int hits{0}, misses{0};
    std::string raw;
    [[nodiscard]] double create_ms() const { return lib_ms + pso_ms + load_ms; }
};

std::string self_executable() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) return {};
    path.resize(std::strlen(path.c_str()));
    std::error_code ec;
    auto canon = std::filesystem::canonical(path, ec);
    return ec ? path : canon.string();
}

// Re-execs `exe` (this test binary, or a hard link to it inside a temp .app)
// running only `child_test`, with `env` prepended, and parses its WS4RESULT.
RunResult run_child(const std::string& exe, const std::string& child_test,
                    const std::vector<std::string>& env) {
    std::string cmd = "/usr/bin/env";
    for (const auto& kv : env) cmd += " '" + kv + "'";
    cmd += " '" + exe + "' --gtest_also_run_disabled_tests --gtest_filter=" + child_test +
           " 2>&1";
    RunResult r;
    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) return r;
    char line[4096];
    while (std::fgets(line, sizeof line, p)) {
        r.raw += line;
        if (std::strncmp(line, "WS4RESULT ", 10) == 0) {
            r.ok = std::sscanf(line,
                               "WS4RESULT lib_ms=%lf pso_ms=%lf load_ms=%lf add_ms=%lf "
                               "ser_ms=%lf hits=%d misses=%d",
                               &r.lib_ms, &r.pso_ms, &r.load_ms, &r.add_ms, &r.ser_ms,
                               &r.hits, &r.misses) == 7;
        }
    }
    const int status = ::pclose(p);
    if (status != 0) r.ok = false;
    return r;
}

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

std::string darwin_user_cache_dir() {
    char buf[PATH_MAX];
    const size_t n = ::confstr(_CS_DARWIN_USER_CACHE_DIR, buf, sizeof buf);
    if (n == 0 || n > sizeof buf) return {};
    return buf;
}

// A temp .app wrapping a hard link to this binary, so the child runs under its
// own CFBundleIdentifier and therefore its own (empty) Metal cache. Same shape
// as prepare_guest_app_bundle in src/main.cpp.
struct TempBundle {
    std::filesystem::path root;
    std::filesystem::path exe;
    std::string bundle_id;
    TempBundle(const std::string& self, const std::string& id) : bundle_id(id) {
        namespace fs = std::filesystem;
        root = fs::path(self).parent_path() / ("ws4-bundle-" + id);
        const fs::path macos = root / "WS4Probe.app" / "Contents" / "MacOS";
        fs::create_directories(macos);
        exe = macos / "WS4Probe";
        std::error_code ec;
        fs::create_hard_link(self, exe, ec);
        if (ec) fs::copy_file(self, exe, fs::copy_options::overwrite_existing);
        std::ofstream plist(root / "WS4Probe.app" / "Contents" / "Info.plist");
        plist << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                 "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                 "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                 "<plist version=\"1.0\"><dict>\n"
                 "<key>CFBundleExecutable</key><string>WS4Probe</string>\n"
                 "<key>CFBundleIdentifier</key><string>"
              << id
              << "</string>\n"
                 "<key>CFBundlePackageType</key><string>APPL</string>\n"
                 "<key>CFBundleVersion</key><string>1</string>\n"
                 "</dict></plist>\n";
    }
    ~TempBundle() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        const std::string cache = darwin_user_cache_dir();
        if (!cache.empty() && !bundle_id.empty()) {
            std::filesystem::remove_all(std::filesystem::path(cache) / bundle_id, ec);
        }
    }
};

}  // namespace ws4bench

using namespace ws4bench;

// Child arm: WS4_NONCE, WS4_MODE (none|record|use), WS4_ARCHIVE.
TEST(MetalPipelineArchiveBenchChild, DISABLED_Arm) {
    const char* nonce_env = std::getenv("WS4_NONCE");
    const char* mode_env = std::getenv("WS4_MODE");
    const char* path_env = std::getenv("WS4_ARCHIVE");
    ASSERT_NE(nonce_env, nullptr);
    ASSERT_NE(mode_env, nullptr);
    const unsigned nonce = static_cast<unsigned>(std::strtoul(nonce_env, nullptr, 10));
    const std::string mode = mode_env;
    ASSERT_TRUE(mode == "none" || mode == "record" || mode == "use");
    ASSERT_TRUE(mode == "none" || path_env != nullptr);
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) GTEST_SKIP() << "no Metal device";
        RunResult r;
        id<MTLBinaryArchive> arch = nil;
        const double t0 = now_ms();
        if (mode != "none") {
            MTLBinaryArchiveDescriptor* d = [MTLBinaryArchiveDescriptor new];
            if (mode == "use") d.url = [NSURL fileURLWithPath:@(path_env)];
            NSError* e = nil;
            arch = [dev newBinaryArchiveWithDescriptor:d error:&e];
            ASSERT_NE(arch, nil) << e.localizedDescription.UTF8String;
        }
        r.load_ms = now_ms() - t0;
        for (int i = 0; i < kCorpusPairs; ++i) {
            NSError* e = nil;
            const double a = now_ms();
            id<MTLLibrary> lk = [dev newLibraryWithSource:@(corpus_kernel(nonce, i).c_str())
                                                  options:nil
                                                    error:&e];
            ASSERT_NE(lk, nil) << e.localizedDescription.UTF8String;
            id<MTLLibrary> lr = [dev newLibraryWithSource:@(corpus_render(nonce, i).c_str())
                                                  options:nil
                                                    error:&e];
            ASSERT_NE(lr, nil) << e.localizedDescription.UTF8String;
            const double b = now_ms();
            r.lib_ms += b - a;
            MTLComputePipelineDescriptor* cd = [MTLComputePipelineDescriptor new];
            cd.computeFunction = [lk newFunctionWithName:[NSString stringWithFormat:@"k%d", i]];
            MTLRenderPipelineDescriptor* rd = [MTLRenderPipelineDescriptor new];
            rd.vertexFunction = [lr newFunctionWithName:[NSString stringWithFormat:@"vs%d", i]];
            rd.fragmentFunction = [lr newFunctionWithName:[NSString stringWithFormat:@"fs%d", i]];
            rd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            id c = nil, rp = nil;
            if (mode == "use") {
                cd.binaryArchives = @[ arch ];
                rd.binaryArchives = @[ arch ];
                c = [dev newComputePipelineStateWithDescriptor:cd
                                                       options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                                    reflection:nil
                                                         error:&e];
                rp = [dev newRenderPipelineStateWithDescriptor:rd
                                                       options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                                    reflection:nil
                                                         error:&e];
                if (c) ++r.hits; else {
                    ++r.misses;
                    cd.binaryArchives = nil;
                    c = [dev newComputePipelineStateWithDescriptor:cd options:0 reflection:nil error:&e];
                }
                if (rp) ++r.hits; else {
                    ++r.misses;
                    rd.binaryArchives = nil;
                    rp = [dev newRenderPipelineStateWithDescriptor:rd options:0 reflection:nil error:&e];
                }
            } else {
                c = [dev newComputePipelineStateWithDescriptor:cd options:0 reflection:nil error:&e];
                rp = [dev newRenderPipelineStateWithDescriptor:rd options:0 reflection:nil error:&e];
            }
            ASSERT_NE(c, nil) << e.localizedDescription.UTF8String;
            ASSERT_NE(rp, nil) << e.localizedDescription.UTF8String;
            r.pso_ms += now_ms() - b;
            if (mode == "record") {
                const double x = now_ms();
                ASSERT_TRUE([arch addComputePipelineFunctionsWithDescriptor:cd error:&e]);
                ASSERT_TRUE([arch addRenderPipelineFunctionsWithDescriptor:rd error:&e]);
                r.add_ms += now_ms() - x;
            }
        }
        if (mode == "record") {
            const double x = now_ms();
            NSError* e = nil;
            ASSERT_TRUE([arch serializeToURL:[NSURL fileURLWithPath:@(path_env)] error:&e])
                << e.localizedDescription.UTF8String;
            r.ser_ms = now_ms() - x;
        }
        std::printf("WS4RESULT lib_ms=%.3f pso_ms=%.3f load_ms=%.3f add_ms=%.3f ser_ms=%.3f "
                    "hits=%d misses=%d\n",
                    r.lib_ms, r.pso_ms, r.load_ms, r.add_ms, r.ser_ms, r.hits, r.misses);
        std::fflush(stdout);
    }
}

TEST(MetalPipelineArchiveBench, DISABLED_UpperBound) {
    if (!MTLCreateSystemDefaultDevice()) GTEST_SKIP() << "no Metal device";
    const std::string self = self_executable();
    ASSERT_FALSE(self.empty());
    const std::string child = "MetalPipelineArchiveBenchChild.DISABLED_Arm";
    const std::filesystem::path work =
        std::filesystem::path(self).parent_path() /
        ("ws4-probe-" + std::to_string(static_cast<int>(::getpid())));
    std::filesystem::create_directories(work);

    std::random_device rd;
    std::mt19937 rng(rd());
    constexpr int kReps = 5;
    std::vector<double> arm[7];
    std::vector<double> add_ms, ser_ms;
    for (int rep = 0; rep < kReps; ++rep) {
        const unsigned a = rng() % 1000000000u, b = rng() % 1000000000u,
                       c = rng() % 1000000000u;
        auto env = [](unsigned n, const char* mode, const std::string& path) {
            return std::vector<std::string>{"WS4_NONCE=" + std::to_string(n),
                                            std::string("WS4_MODE=") + mode,
                                            "WS4_ARCHIVE=" + path};
        };
        const std::string pb = (work / ("b-" + std::to_string(rep) + ".bin")).string();
        const std::string pc = (work / ("c-" + std::to_string(rep) + ".bin")).string();

        RunResult r1 = run_child(self, child, env(a, "none", ""));
        RunResult r2 = run_child(self, child, env(a, "none", ""));
        RunResult r3 = run_child(self, child, env(b, "record", pb));
        RunResult r4 = run_child(self, child, env(b, "use", pb));
        RunResult r5, r6;
        {
            TempBundle rec(self, "dev.vitreo.test.ws4.r" + std::to_string(c));
            r5 = run_child(rec.exe.string(), child, env(c, "record", pc));
        }
        {
            TempBundle use(self, "dev.vitreo.test.ws4.u" + std::to_string(c));
            r6 = run_child(use.exe.string(), child, env(c, "use", pc));
        }
        const RunResult* all[] = {&r1, &r2, &r3, &r4, &r5, &r6};
        for (int i = 0; i < 6; ++i) {
            ASSERT_TRUE(all[i]->ok) << "arm " << (i + 1) << ":\n" << all[i]->raw;
            arm[i + 1].push_back(all[i]->create_ms());
        }
        ASSERT_EQ(r4.misses, 0) << r4.raw;
        ASSERT_EQ(r4.hits, 2 * kCorpusPairs);
        ASSERT_EQ(r6.misses, 0) << r6.raw;
        add_ms.push_back(r3.add_ms);
        ser_ms.push_back(r3.ser_ms);
        std::printf("rep %d: cold=%.1f warm-sys=%.1f record=%.1f warm-arch=%.1f "
                    "rec-fresh=%.1f arch-only=%.1f (arm6 lib=%.1f vs arm2 lib=%.1f)\n",
                    rep, r1.create_ms(), r2.create_ms(), r3.create_ms(), r4.create_ms(),
                    r5.create_ms(), r6.create_ms(), r6.lib_ms, r2.lib_ms);
        // Arm 6 must really be system-cold, or the bundle trick did not
        // isolate Metal's per-app cache and arm 6 means nothing.
        EXPECT_GT(r6.lib_ms, 10 * r2.lib_ms) << "bundle id did not isolate the Metal cache";
    }
    std::error_code ec;
    std::filesystem::remove_all(work, ec);

    const char* names[] = {"", "cold", "warm-system", "record", "warm-archive",
                           "record (fresh bundle)", "archive, system cold"};
    std::printf("WS4 upper bound, %d pipelines/process, median of %d (ms, lib+pso+load):\n",
                2 * kCorpusPairs, kReps);
    for (int i = 1; i <= 6; ++i) std::printf("  arm %d %-22s %10.2f\n", i, names[i], median(arm[i]));
    std::printf("  record add %.2f, serialize %.2f\n", median(add_ms), median(ser_ms));
    const double ratio = median(arm[4]) / median(arm[2]);
    std::printf("  warm-archive / warm-system = %.3f -> %s\n", ratio,
                ratio <= 0.97 ? "GO" : "NO-GO");
}
