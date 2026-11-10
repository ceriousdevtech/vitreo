// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_backend_pipeline.mm
/// @brief Shader compilation and render pipeline state

#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/CAMetalLayer.h>
#include "metal_device.hpp"
#include "graphics/shaders/compile_profile.hpp"
#include "graphics/common/diagnostic_path.hpp"
#include "graphics/metal/metal_buffer.hpp"
#include "graphics/metal/metal_swapchain.hpp"
#include "graphics/metal/metal_submit_queue.hpp"
#include "graphics/metal/metal_tiled.hpp"
#include "graphics/metal/metal_gpu_profile.hpp"
#include "graphics/metal/metal_shader_validation.hpp"
#include "graphics/contracts/delta_record.hpp"
#include "graphics/contracts/slim_record.hpp"
#include <vitreo/ab_cycle.hpp>

#include <atomic>
#include <cctype>
#include <cmath>
#include <chrono>
#include "graphics/metal/metal_texture.hpp"
#include "graphics/metal/metal_shared_resource.hpp"
#include "core/memory/host_memory_floor.hpp"

#include <mach/mach.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <limits>
#include <memory>
#include <map>
#include <unordered_map>
#include <utility>
#include <mutex>
#include <thread>
#include <deque>
#include <unistd.h>
#include <condition_variable>
#include <vector>

#include "graphics/metal/metal_backend_enums.hpp"
#include "graphics/metal/metal_backend_pipeline.hpp"
#include "graphics/shaders/msl_point_size.hpp"

using namespace vitreo;

namespace vitreo::graphics {

namespace {

std::string link_missing_fragment_inputs(std::string vertex,
                                         const std::string& fragment) {
    const usize output_struct = vertex.find("struct main0_out");
    if (output_struct == std::string::npos) return vertex;
    const usize output_end = vertex.find("};", output_struct);
    if (output_end == std::string::npos) return vertex;

    std::string additions;
    usize cursor = 0;
    constexpr std::string_view marker = "[[user(locn";
    while ((cursor = fragment.find(marker, cursor)) != std::string::npos) {
        // The location is the digits after `locn`. A fragment input may carry
        // an interpolation qualifier -- `[[user(locn0), center_no_perspective]]`
        // -- so the attribute does not necessarily close right after them.
        const usize digits = cursor + marker.size();
        usize digits_end = digits;
        while (digits_end < fragment.size() &&
               fragment[digits_end] >= '0' && fragment[digits_end] <= '9') {
            ++digits_end;
        }
        if (digits_end == digits || digits_end >= fragment.size() ||
            fragment[digits_end] != ')') {
            cursor = digits_end;
            continue;
        }
        const std::string location = fragment.substr(digits, digits_end - digits);
        // A vertex output never takes an interpolation qualifier.
        const std::string attribute =
            "[[user(locn" + location + ")]]";
        const std::string existing = "[[user(locn" + location + ")";
        const usize close = digits_end;
        if (vertex.find(existing, output_struct) == std::string::npos) {
            const usize declaration_start =
                fragment.find_last_of("\n;{", cursor);
            std::string declaration = fragment.substr(
                declaration_start == std::string::npos
                    ? 0 : declaration_start + 1,
                cursor - (declaration_start == std::string::npos
                              ? 0 : declaration_start + 1));
            while (!declaration.empty() &&
                   (declaration.back() == ' ' || declaration.back() == '\t')) {
                declaration.pop_back();
            }
            const usize name_start = declaration.find_last_of(" \t");
            if (name_start != std::string::npos) {
                declaration.resize(name_start + 1);
                additions += "\n" + declaration + "vitreo_link_pad_" + location +
                    " " + attribute + ";\n";
            }
        }
        cursor = close;
    }
    if (!additions.empty()) vertex.insert(output_end, additions);
    return vertex;
}

MTLVertexFormat metal_vertex_format(BackendVertexFormat format) {
    switch (format) {
    case BackendVertexFormat::Invalid: return MTLVertexFormatInvalid;
    case BackendVertexFormat::Float1: return MTLVertexFormatFloat;
    case BackendVertexFormat::Float2: return MTLVertexFormatFloat2;
    case BackendVertexFormat::Float3: return MTLVertexFormatFloat3;
    case BackendVertexFormat::Float4: return MTLVertexFormatFloat4;
    case BackendVertexFormat::UByte4NormalizedBGRA:
        // The shader receives RGBA; a D3DCOLOR is stored BGRA in memory, so
        // the swizzled Metal format is what makes the two agree.
        return MTLVertexFormatUChar4Normalized_BGRA;
    case BackendVertexFormat::UByte4Normalized:
        return MTLVertexFormatUChar4Normalized;
    case BackendVertexFormat::UByte4:
        return MTLVertexFormatUChar4;
    case BackendVertexFormat::Short2:
        return MTLVertexFormatShort2;
    case BackendVertexFormat::Short4:
        return MTLVertexFormatShort4;
    case BackendVertexFormat::Short2Normalized:
        return MTLVertexFormatShort2Normalized;
    case BackendVertexFormat::Short4Normalized:
        return MTLVertexFormatShort4Normalized;
    case BackendVertexFormat::UShort2Normalized:
        return MTLVertexFormatUShort2Normalized;
    case BackendVertexFormat::UShort2:
        return MTLVertexFormatUShort2;
    case BackendVertexFormat::UShort1:
        return MTLVertexFormatUShort;
    case BackendVertexFormat::UShort4:
        return MTLVertexFormatUShort4;
    case BackendVertexFormat::Short1:
        return MTLVertexFormatShort;
    case BackendVertexFormat::UShort4Normalized:
        return MTLVertexFormatUShort4Normalized;
    case BackendVertexFormat::Half2:
        return MTLVertexFormatHalf2;
    case BackendVertexFormat::Half4:
        return MTLVertexFormatHalf4;
    case BackendVertexFormat::Half1:
        return MTLVertexFormatHalf;
    case BackendVertexFormat::UInt1: return MTLVertexFormatUInt;
    case BackendVertexFormat::UInt2: return MTLVertexFormatUInt2;
    case BackendVertexFormat::UInt3: return MTLVertexFormatUInt3;
    case BackendVertexFormat::UInt4: return MTLVertexFormatUInt4;
    case BackendVertexFormat::SInt1: return MTLVertexFormatInt;
    case BackendVertexFormat::SInt2: return MTLVertexFormatInt2;
    case BackendVertexFormat::SInt3: return MTLVertexFormatInt3;
    case BackendVertexFormat::SInt4: return MTLVertexFormatInt4;
    case BackendVertexFormat::UByte1: return MTLVertexFormatUChar;
    case BackendVertexFormat::UByte2: return MTLVertexFormatUChar2;
    case BackendVertexFormat::SByte1: return MTLVertexFormatChar;
    case BackendVertexFormat::SByte2: return MTLVertexFormatChar2;
    case BackendVertexFormat::SByte4: return MTLVertexFormatChar4;
    case BackendVertexFormat::UByte1Normalized:
        return MTLVertexFormatUCharNormalized;
    case BackendVertexFormat::UByte2Normalized:
        return MTLVertexFormatUChar2Normalized;
    case BackendVertexFormat::SByte1Normalized:
        return MTLVertexFormatCharNormalized;
    case BackendVertexFormat::SByte2Normalized:
        return MTLVertexFormatChar2Normalized;
    case BackendVertexFormat::SByte4Normalized:
        return MTLVertexFormatChar4Normalized;
    case BackendVertexFormat::UShort1Normalized:
        return MTLVertexFormatUShortNormalized;
    case BackendVertexFormat::Short1Normalized:
        return MTLVertexFormatShortNormalized;
    case BackendVertexFormat::UInt1010102Normalized:
    case BackendVertexFormat::UInt1010102:
        return MTLVertexFormatUInt1010102Normalized;
    case BackendVertexFormat::FloatRG11B10:
        return MTLVertexFormatFloatRG11B10;
    case BackendVertexFormat::FloatRGB9E5:
        return MTLVertexFormatFloatRGB9E5;
    case BackendVertexFormat::Dec3N:
        // D3DDECLTYPE_DEC3N is signed 10:10:10 normalized (w forced to 1).
        // Metal's matching packed format is Int1010102Normalized.
        return MTLVertexFormatInt1010102Normalized;
    }
    return MTLVertexFormatInvalid;
}

} // namespace

/// A stable name for a shader: gpu_watch reports pipelines by label, and
/// VITREO_DUMP_MSL_DIR writes the source under the same name, so a stuck
/// command buffer's contents lead straight to the shader text.
std::string shader_tag(const char* stage, const std::string& msl) {
    char tag[40];
    std::snprintf(tag, sizeof(tag), "%s:%016llx", stage,
                  static_cast<unsigned long long>(std::hash<std::string>{}(msl)));
    static const char* dump_dir = std::getenv("VITREO_DUMP_MSL_DIR");
    if (dump_dir != nullptr && !msl.empty()) {
        std::string file = tag;
        std::replace(file.begin(), file.end(), ':', '_');
        const std::string path = std::string(dump_dir) + "/" + file + ".metal";
        if (std::FILE* f = std::fopen(path.c_str(), "wx")) {
            std::fwrite(msl.data(), 1, msl.size(), f);
            std::fclose(f);
        }
    }
    return tag;
}

/// MTLCompileOptions for a translated shader's math contract.
/// VITREO_METAL_MATH_MODE=fast|relaxed|safe overrides the frontend's choice
/// for every library (an A/B diagnostic, not a setting).
[[nodiscard]] MTLCompileOptions* metal_compile_options(BackendMathMode mode,
                                                        bool invariance) {
    static const int forced = [] {
        const char* text = std::getenv("VITREO_METAL_MATH_MODE");
        if (text == nullptr) return -1;
        if (std::strcmp(text, "fast") == 0) return 0;
        if (std::strcmp(text, "relaxed") == 0) return 1;
        if (std::strcmp(text, "safe") == 0) return 2;
        return -1;
    }();
    if (forced >= 0) mode = static_cast<BackendMathMode>(forced);
    MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
    if (@available(macOS 15.0, *)) {
        options.mathMode = mode == BackendMathMode::Safe ? MTLMathModeSafe
                           : mode == BackendMathMode::Relaxed
                               ? MTLMathModeRelaxed
                               : MTLMathModeFast;
    } else {
        // Before mathMode there is only fast math on or off; off is the
        // IEEE-preserving choice for both Relaxed and Safe.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        options.fastMathEnabled = mode == BackendMathMode::Fast;
#pragma clang diagnostic pop
    }
    options.preserveInvariance = invariance ? YES : NO;
    return options;
}

namespace {

/// VITREO_METAL_COMPILE_SALT=<text>, as an identifier; empty when unset.
const std::string& compile_salt() {
    static const std::string salt = [] {
        const char* text = std::getenv("VITREO_METAL_COMPILE_SALT");
        std::string ident;
        if (text == nullptr) return ident;
        for (const char* c = text; *c != '\0'; ++c) {
            ident += std::isalnum(static_cast<unsigned char>(*c)) ? *c : '_';
        }
        return ident;
    }();
    return salt;
}

}  // namespace

NSString* metal_shader_source(const std::string& msl, const std::string& entry) {
    const unsigned active = enabled_sampler_features() & sampler_features_in_msl(msl);
    std::string source;
    if ((active & static_cast<unsigned>(SamplerFeature::MinMax)) != 0)
        source += std::string("#define ") + kMinMaxReductionMacro + " 1\n";
    if ((active & static_cast<unsigned>(SamplerFeature::CustomBorder)) != 0)
        source += std::string("#define ") + kCustomBorderMacro + " 1\n";
    if ((active & static_cast<unsigned>(SamplerFeature::NonNormalized)) != 0)
        source += std::string("#define ") + kNonNormalizedMacro + " 1\n";
    if (!source.empty()) {
        if (!compile_salt().empty() && !entry.empty())
            source += "#define " + entry + " " + entry + "_s" + compile_salt() + "\n";
        source += msl;
        return @(source.c_str());
    }
    // VITREO_METAL_COMPILE_SALT=<text>: the entry point is renamed
    // <entry>_s<text>, so the compiler service has seen neither the source
    // (its library cache) nor the function's AIR (its function cache, which
    // an unused extra declaration does not change) -- honest cold-compile
    // numbers for tests/tools/d3d12_pso_replay. A benchmark knob, not a
    // setting.
    if (compile_salt().empty() || entry.empty()) return @(msl.c_str());
    return @(("#define " + entry + " " + entry + "_s" + compile_salt() + "\n" + msl).c_str());
}

NSString* metal_entry_name(const std::string& entry) {
    if (compile_salt().empty() || entry.empty()) return @(entry.c_str());
    return @((entry + "_s" + compile_salt()).c_str());
}

namespace {

/// Compiled shader functions, shared by every pipeline built from the same
/// MSL.
///
/// D3D11 has no pipeline object: a Metal render pipeline is formed per
/// (vertex shader, pixel shader, state) at draw time, and each one used to
/// compile BOTH of its shaders from source again. A D3D11 renderer pairs a
/// pixel shader with many vertex shaders and render states, so most of those
/// compiles repeated one already done, synchronously on the render thread.
/// Loading a D3D11 world, that thread spent about half its time in these
/// compiles (63% in newLibraryWithSource, 37% in pipeline state creation).
///
/// Keyed by the exact source (two independent 64-bit hashes and the length),
/// the entry point and the compile options. A function is immutable and valid
/// for any number of pipelines.
class MetalFunctionCache {
public:
    static MetalFunctionCache& instance() {
        static MetalFunctionCache* cache = new MetalFunctionCache();
        return *cache;
    }

    /// The function @p entry of @p source, compiled once. With @p wait, a
    /// prefetch still compiling is waited on, and one still queued is compiled
    /// here. Without @p wait the caller never blocks: a miss is queued at the
    /// front and this returns nil with an empty @p failure (not an error).
    /// nil with a message is a compile failure; failures are not cached.
    id<MTLFunction> get(id<MTLDevice> device, const std::string& source,
                        const std::string& entry, BackendMathMode math_mode,
                        bool invariance, bool wait, std::string& failure) {
        const Key key = make_key(source, entry, math_mode, invariance);
        if (!wait) {
            prefetch(device, source, entry, math_mode, invariance, true);
            std::shared_ptr<Slot> ready_slot;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_functions.find(key);
                if (it == m_functions.end()) return nil;
                ready_slot = it->second;
            }
            std::lock_guard<std::mutex> lock(ready_slot->mutex);
            if (!ready_slot->ready) return nil;
            if (ready_slot->function == nil) failure = ready_slot->failure;
            else m_reused.fetch_add(1, std::memory_order_relaxed);
            return ready_slot->function;
        }
        std::shared_ptr<Slot> slot;
        bool compile_here = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_functions.find(key);
            if (it != m_functions.end()) {
                slot = it->second;
            } else {
                slot = std::make_shared<Slot>();
                m_functions.emplace(key, slot);
                compile_here = true;
            }
        }
        if (!compile_here) {
            // A queued prefetch stays on the compile pool. Compiling it here
            // runs newLibraryWithSource on the draw thread, which stalls that
            // thread and the pool's completion handlers together. Promote the
            // job and wait for the pool instead.
            bool queued = false;
            {
                std::lock_guard<std::mutex> queue_lock(m_queue_mutex);
                queued = slot->queued;
            }
            if (queued) {
                m_claimed.fetch_add(1, std::memory_order_relaxed);
                promote(slot);
                pump(device);
            }
        }
        if (!compile_here) {
            std::unique_lock<std::mutex> lock(slot->mutex);
            if (slot->ready) {
                m_reused.fetch_add(1, std::memory_order_relaxed);
            } else {
                const auto t0 = std::chrono::steady_clock::now();
                slot->cv.wait(lock, [&] { return slot->ready; });
                m_waited.fetch_add(1, std::memory_order_relaxed);
                m_wait_ns.fetch_add(elapsed_ns(t0), std::memory_order_relaxed);
            }
            if (slot->function == nil) failure = slot->failure;
            return slot->function;
        }
        const auto t0 = std::chrono::steady_clock::now();
        NSError* error = nil;
        compile_profile::begin(compile_profile::MetalLibrary);
        id<MTLLibrary> library =
            [device newLibraryWithSource:metal_shader_source(source, entry)
                                 options:metal_compile_options(math_mode,
                                                               invariance)
                                   error:&error];
        compile_profile::end(compile_profile::MetalLibrary, elapsed_ns(t0));
        m_compile_ns.fetch_add(elapsed_ns(t0), std::memory_order_relaxed);
        m_compiled.fetch_add(1, std::memory_order_relaxed);
        finish(key, slot, library, error, entry);
        std::lock_guard<std::mutex> lock(slot->mutex);
        if (slot->function == nil) failure = slot->failure;
        return slot->function;
    }

    /// Start compiling @p source in the background unless it is already
    /// compiled or compiling. @p urgent places a new job at the front and
    /// moves an already-queued one there, so a draw does not wait behind a
    /// warmup burst.
    void prefetch(id<MTLDevice> device, const std::string& source,
                  const std::string& entry, BackendMathMode math_mode,
                  bool invariance, bool urgent = false) {
        if (device == nil || source.empty()) return;
        const Key key = make_key(source, entry, math_mode, invariance);
        auto slot = std::make_shared<Slot>();
        std::shared_ptr<Slot> existing;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_functions.find(key);
            if (it != m_functions.end()) existing = it->second;
            else m_functions.emplace(key, slot);
        }
        if (existing) {
            if (urgent) promote(existing);
            pump(device);
            return;
        }
        m_prefetched.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> queue_lock(m_queue_mutex);
            slot->queued = true;
            Job job{key, slot, source, entry, math_mode, invariance};
            if (urgent) m_queue.push_front(std::move(job));
            else m_queue.push_back(std::move(job));
        }
        pump(device);
    }

    void note_pipeline_state(u64 ns) {
        m_states.fetch_add(1, std::memory_order_relaxed);
        m_state_ns.fetch_add(ns, std::memory_order_relaxed);
    }

    /// VITREO_GPU_PROFILE: a line per interval with work to report.
    void maybe_report() {
        if (!gpu_profile::enabled()) return;
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(m_report_mutex);
        static const double period = [] {
            const char* text = std::getenv("VITREO_GPU_PROFILE_SECONDS");
            const double v = text != nullptr ? std::atof(text) : 5.0;
            return v >= 0.5 ? v : 5.0;
        }();
        if (now - m_last_report < std::chrono::duration<double>(period)) {
            return;
        }
        const double window =
            std::chrono::duration<double>(now - m_last_report).count();
        m_last_report = now;
        const u64 compiled = m_compiled.exchange(0);
        const u64 reused = m_reused.exchange(0);
        const u64 waited = m_waited.exchange(0);
        const u64 prefetched = m_prefetched.exchange(0);
        const u64 claimed = m_claimed.exchange(0);
        const u64 states = m_states.exchange(0);
        const double compile_ms = m_compile_ns.exchange(0) / 1e6;
        const double wait_ms = m_wait_ns.exchange(0) / 1e6;
        const double state_ms = m_state_ns.exchange(0) / 1e6;
        if (compiled == 0 && reused == 0 && waited == 0 && states == 0 &&
            prefetched == 0) {
            return;
        }
        std::fprintf(stderr,
                     "[metal] shader compiles over %.1f s: %llu libraries "
                     "compiled at draw (%.0f ms, %llu of them taken from the "
                     "prefetch queue), %llu ready, %llu waited on a "
                     "prefetch (%.0f ms), %llu prefetches queued; %llu "
                     "pipeline states (%.0f ms)\n",
                     window, static_cast<unsigned long long>(compiled),
                     compile_ms, static_cast<unsigned long long>(claimed),
                     static_cast<unsigned long long>(reused),
                     static_cast<unsigned long long>(waited), wait_ms,
                     static_cast<unsigned long long>(prefetched),
                     static_cast<unsigned long long>(states), state_ms);
    }

private:
    struct Key {
        size_t hash;
        u64 fnv;
        usize length;
        std::string entry;
        u8 math_mode;
        bool invariance;
        bool operator==(const Key& other) const {
            return hash == other.hash && fnv == other.fnv &&
                   length == other.length && entry == other.entry &&
                   math_mode == other.math_mode &&
                   invariance == other.invariance;
        }
    };
    struct KeyHash {
        size_t operator()(const Key& key) const {
            return key.hash ^ (static_cast<size_t>(key.fnv) * 31u) ^
                   std::hash<std::string>{}(key.entry) ^
                   (static_cast<size_t>(key.math_mode) << 1) ^
                   static_cast<size_t>(key.invariance);
        }
    };
    /// One key's function: compiled, being compiled, or failed.
    struct Slot {
        std::mutex mutex;
        std::condition_variable cv;
        bool ready{false};
        id<MTLFunction> function{nil};
        std::string failure;
        /// A prefetch waiting in m_queue, not yet handed to Metal. A get()
        /// that finds it compiles it itself instead (m_queue_mutex).
        bool queued{false};
    };
    /// A prefetch not yet handed to Metal.
    struct Job {
        Key key;
        std::shared_ptr<Slot> slot;
        std::string source;
        std::string entry;
        BackendMathMode math_mode;
        bool invariance;
    };

    /// Compiles Metal runs at once for prefetches. Everything past this waits
    /// in m_queue, where a draw that needs it can take it (see get()): with an
    /// unbounded submission, ~200 prefetches from one burst queued inside the
    /// compiler service and a draw waited 5-8 s for the one it needed.
    static usize max_in_flight() {
        static const usize n = [] {
            const unsigned cores = std::thread::hardware_concurrency();
            return static_cast<usize>(
                std::clamp<unsigned>(cores / 2, 2u, 8u));
        }();
        return n;
    }

    /// A draw asked for a shader that is still sitting in the queue. Compile
    /// it next, without taking the draw thread into newLibraryWithSource.
    void promote(const std::shared_ptr<Slot>& slot) {
        std::lock_guard<std::mutex> queue_lock(m_queue_mutex);
        if (!slot->queued) return;
        const auto it = std::find_if(
            m_queue.begin(), m_queue.end(),
            [&](const Job& job) { return job.slot == slot; });
        if (it == m_queue.end() || it == m_queue.begin()) return;
        Job job = std::move(*it);
        m_queue.erase(it);
        m_queue.push_front(std::move(job));
    }

    /// Hand queued prefetches to Metal while fewer than max_in_flight() run.
    void pump(id<MTLDevice> device) {
        for (;;) {
            Job job;
            {
                std::lock_guard<std::mutex> queue_lock(m_queue_mutex);
                if (m_in_flight >= max_in_flight()) return;
                while (!m_queue.empty() && !m_queue.front().slot->queued) {
                    m_queue.pop_front();  // claimed by a get()
                }
                if (m_queue.empty()) return;
                job = std::move(m_queue.front());
                m_queue.pop_front();
                job.slot->queued = false;
                ++m_in_flight;
            }
            const Key key = job.key;
            const std::shared_ptr<Slot> slot = job.slot;
            const std::string entry = job.entry;
            const auto submitted = std::chrono::steady_clock::now();
            compile_profile::begin(compile_profile::MetalLibrary);
            [device newLibraryWithSource:metal_shader_source(job.source, job.entry)
                                 options:metal_compile_options(job.math_mode,
                                                               job.invariance)
                       completionHandler:^(id<MTLLibrary> library,
                                           NSError* error) {
                         compile_profile::end(compile_profile::MetalLibrary,
                                              elapsed_ns(submitted));
                         finish(key, slot, library, error, entry);
                         {
                             std::lock_guard<std::mutex> queue_lock(
                                 m_queue_mutex);
                             --m_in_flight;
                         }
                         pump(device);
                       }];
        }
    }

    static Key make_key(const std::string& source, const std::string& entry,
                        BackendMathMode math_mode, bool invariance) {
        // The MIN/MAX switch compiles the same source two ways.
        const u64 features = enabled_sampler_features() & sampler_features_in_msl(source);
        return Key{std::hash<std::string>{}(source),
                   fnv1a(source) ^ (features * 0x4D494E4D41580001ull),
                   source.size(), entry, static_cast<u8>(math_mode),
                   invariance};
    }
    static u64 fnv1a(const std::string& text) {
        u64 h = 1469598103934665603ull;
        for (const unsigned char c : text) {
            h ^= c;
            h *= 1099511628211ull;
        }
        return h;
    }
    static u64 elapsed_ns(std::chrono::steady_clock::time_point t0) {
        return static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0)
                .count());
    }

    /// Publish a finished compile. A failure leaves the map, so the next
    /// request compiles again and reports the error itself.
    void finish(const Key& key, const std::shared_ptr<Slot>& slot,
                id<MTLLibrary> library, NSError* error,
                const std::string& entry) {
        id<MTLFunction> function =
            library != nil ? [library newFunctionWithName:metal_entry_name(entry)]
                           : nil;
        if (function == nil) {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_functions.find(key);
            if (it != m_functions.end() && it->second == slot) {
                m_functions.erase(it);
            }
        }
        {
            std::lock_guard<std::mutex> lock(slot->mutex);
            slot->function = function;
            if (function == nil) {
                slot->failure =
                    library == nil
                        ? (error != nil
                               ? [[error localizedDescription] UTF8String]
                               : "unknown error")
                        : "entry point '" + entry + "' not found";
            }
            slot->ready = true;
        }
        slot->cv.notify_all();
    }

    std::mutex m_mutex;
    std::unordered_map<Key, std::shared_ptr<Slot>, KeyHash> m_functions;
    std::mutex m_queue_mutex;
    std::deque<Job> m_queue;
    usize m_in_flight{0};
    std::atomic<u64> m_claimed{0};
    std::atomic<u64> m_compiled{0};
    std::atomic<u64> m_reused{0};
    std::atomic<u64> m_waited{0};
    std::atomic<u64> m_prefetched{0};
    std::atomic<u64> m_compile_ns{0};
    std::atomic<u64> m_wait_ns{0};
    std::atomic<u64> m_states{0};
    std::atomic<u64> m_state_ns{0};
    std::mutex m_report_mutex;
    std::chrono::steady_clock::time_point m_last_report{
        std::chrono::steady_clock::now()};
};

/// Synchronous Windows PSO creation must return a finished native state. Caller
/// object identities are not native pipeline state: share exact descriptions,
/// with per-key singleflight, without holding the map mutex during compilation.
class SynchronousRenderStateCache {
    struct Key {
        void* device{nullptr};
        void* vertex{nullptr};
        void* fragment{nullptr};
        BackendPipelineDesc shape;
        bool operator==(const Key&) const = default;
    };
    struct Hash {
        usize operator()(const Key& key) const noexcept {
            // Full equality decides every hit. A small hash is only a bucket
            // selector; caller hashes and object serials never decide equality.
            usize hash = std::hash<void*>{}(key.device);
            const auto add = [&](usize value) { hash ^= value + (hash << 6) + (hash >> 2); };
            add(std::hash<void*>{}(key.vertex));
            add(std::hash<void*>{}(key.fragment));
            add(key.shape.sample_count);
            add(static_cast<usize>(key.shape.color_format));
            add(static_cast<usize>(key.shape.depth_format));
            return hash;
        }
    };
    struct Slot {
        std::mutex mutex;
        std::condition_variable cv;
        std::atomic<bool> ready{false};
        // Retain pointer identities for the entire cache entry lifetime.
        id<MTLDevice> device;
        id<MTLFunction> vertex;
        id<MTLFunction> fragment;
        id<MTLRenderPipelineState> state;
        NSError* error;
    };
    struct Entry {
        std::shared_ptr<Slot> slot;
        u64 used{0};
    };
public:
    static SynchronousRenderStateCache& instance() {
        static auto* cache = new SynchronousRenderStateCache();
        return *cache;
    }
    id<MTLRenderPipelineState> acquire(id<MTLDevice> device,
        MTLRenderPipelineDescriptor* pipeline, const BackendPipelineDesc& desc,
        NSError** error) {
        const auto build = [&] {
            const compile_profile::Scope timed(compile_profile::MetalRenderState);
            return [device newRenderPipelineStateWithDescriptor:pipeline error:error];
        };
        static const bool enabled = [] {
            const char* value = std::getenv("VITREO_METAL_PSO_CACHE");
            return value == nullptr || value[0] != '0';
        }();
        if (!enabled) {
            compile_profile::note(compile_profile::MetalRenderMiss);
            return build();
        }
        Key key{(__bridge void*)device, (__bridge void*)pipeline.vertexFunction,
                (__bridge void*)pipeline.fragmentFunction, desc};
        // Compiled function identity includes source, entry point, compile
        // options and enabled sampler gates. Avoid retaining duplicate MSL.
        std::string{}.swap(key.shape.vertex_shader_msl);
        std::string{}.swap(key.shape.fragment_shader_msl);
        std::string{}.swap(key.shape.vertex_entry_point);
        std::string{}.swap(key.shape.fragment_entry_point);
        key.shape.cache_key = 0;
        key.shape.synchronous_compile = true;
        std::shared_ptr<Slot> slot;
        bool create = false;
        {
            std::lock_guard lock(m_mutex);
            const auto found = m_states.find(key);
            if (found != m_states.end()) {
                slot = found->second.slot;
                found->second.used = ++m_clock;
            } else {
                // Bound retained native programs. In-flight builders/waiters
                // cannot be evicted; callers retain finished states themselves.
                while (m_states.size() >= kRetainedStates) {
                    auto oldest = m_states.end();
                    for (auto it = m_states.begin(); it != m_states.end(); ++it) {
                        if (it->second.slot.use_count() != 1 ||
                            !it->second.slot->ready.load(std::memory_order_acquire)) continue;
                        if (oldest == m_states.end() || it->second.used < oldest->second.used)
                            oldest = it;
                    }
                    if (oldest == m_states.end()) break;
                    m_states.erase(oldest);
                }
                slot = std::make_shared<Slot>();
                slot->device = device;
                slot->vertex = pipeline.vertexFunction;
                slot->fragment = pipeline.fragmentFunction;
                m_states.emplace(key, Entry{slot, ++m_clock});
                create = true;
            }
        }
        compile_profile::note(create ? compile_profile::MetalRenderMiss
                                     : compile_profile::MetalRenderHit);
        if (create) {
            id<MTLRenderPipelineState> state = build();
            {
                std::lock_guard lock(slot->mutex);
                slot->state = state;
                slot->error = error != nullptr ? *error : nil;
                slot->ready.store(true, std::memory_order_release);
            }
            slot->cv.notify_all();
            if (state == nil) {
                // A resource/driver failure is retryable. Existing waiters keep
                // their Slot and receive the same completed failure.
                std::lock_guard lock(m_mutex);
                const auto found = m_states.find(key);
                if (found != m_states.end() && found->second.slot == slot) m_states.erase(found);
            }
        }
        std::unique_lock lock(slot->mutex);
        slot->cv.wait(lock, [&] { return slot->ready.load(std::memory_order_acquire); });
        if (error != nullptr) *error = slot->error;
        return slot->state;
    }
private:
    static constexpr usize kRetainedStates = 512;
    std::mutex m_mutex;
    std::unordered_map<Key, Entry, Hash> m_states;
    u64 m_clock{0};
};

/// Pipeline states for draws that must not stall. The first request starts
/// newRenderPipelineStateWithDescriptor's completion handler and returns nil.
/// A later request with the same cache key takes the finished state.
class RenderStateCache {
public:
    static RenderStateCache& instance() {
        static RenderStateCache* cache = new RenderStateCache();
        return *cache;
    }

    /// nil while the state is still being built. A finished failure stays nil
    /// and @p failed is set so the caller does not retry forever.
    id<MTLRenderPipelineState> acquire(u64 key, id<MTLDevice> device,
                                      MTLRenderPipelineDescriptor* pipeline,
                                      bool& failed) {
        failed = false;
        std::shared_ptr<Slot> slot;
        bool start = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_states.find(key);
            if (it != m_states.end()) {
                slot = it->second;
            } else {
                slot = std::make_shared<Slot>();
                m_states.emplace(key, slot);
                start = true;
            }
        }
        {
            std::lock_guard<std::mutex> lock(slot->mutex);
            if (slot->ready) {
                failed = slot->state == nil;
                return slot->state;
            }
        }
        if (!start) return nil;
        [device newRenderPipelineStateWithDescriptor:pipeline
                                   completionHandler:^(
                                       id<MTLRenderPipelineState> state,
                                       NSError* error) {
                                     if (state == nil) {
                                         std::fprintf(
                                             stderr,
                                             "[metal] pipeline creation failed: %s\n",
                                             error != nil
                                                 ? [[error localizedDescription]
                                                        UTF8String]
                                                 : "unknown error");
                                     }
                                     {
                                         std::lock_guard<std::mutex> lock(
                                             slot->mutex);
                                         slot->state = state;
                                         slot->ready = true;
                                     }
                                   }];
        return nil;
    }

private:
    struct Slot {
        std::mutex mutex;
        bool ready{false};
        id<MTLRenderPipelineState> state{nil};
    };
    std::mutex m_mutex;
    std::unordered_map<u64, std::shared_ptr<Slot>> m_states;
};

/// The vertex source a pipeline of @p desc compiles: linked to its fragment
/// shader's inputs, and for a point pipeline writing [[point_size]] = 1.0
/// when the shader names none (msl_point_size.hpp, D3D11.3 3.4.6).
std::string linked_vertex_source(const BackendPipelineDesc& desc) {
    return desc.input_primitive_class == BackendPrimitiveClass::Point
               ? msl_with_unit_point_size(link_missing_fragment_inputs(
                     desc.vertex_shader_msl, desc.fragment_shader_msl))
               : link_missing_fragment_inputs(desc.vertex_shader_msl,
                                              desc.fragment_shader_msl);
}

} // namespace

MetalBackendPipeline::MetalBackendPipeline(void* device,
                                           const BackendPipelineDesc& desc) {
    @autoreleasepool {
        id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device;
        if (mtl_device == nil) return;
        const std::string linked_vertex_msl = linked_vertex_source(desc);
        // Read once: the functions below compile with it, and a pipeline
        // built without the MIN/MAX reduction rebuilds itself later.
        const unsigned source_features = sampler_features_in_msl(linked_vertex_msl) |
                                           sampler_features_in_msl(desc.fragment_shader_msl);
        const unsigned compiled_features = enabled_sampler_features();

        const auto compile = [&](const std::string& source,
                                 const std::string& entry_point,
                                 BackendMathMode math_mode,
                                 bool invariance)
            -> id<MTLFunction> {
            if (source.empty()) return nil;
            std::string failure;
            id<MTLFunction> function = MetalFunctionCache::instance().get(
                mtl_device, source, entry_point, math_mode, invariance,
                desc.synchronous_compile, failure);
            if (function == nil && !failure.empty()) {
                std::fprintf(stderr,
                             "[metal] shader compilation failed: %s\n",
                             failure.c_str());
            }
            return function;
        };

        // A new material compiles both stages. Start them together so the
        // draw waits for the slower one, not for their sum. get() below
        // reuses a compile already running and only compiles inline when
        // the prefetch is still sitting in the queue.
        if (!linked_vertex_msl.empty()) {
            MetalFunctionCache::instance().prefetch(
                mtl_device, linked_vertex_msl, desc.vertex_entry_point,
                desc.vertex_math_mode, desc.preserve_invariance);
        }
        if (desc.rasterization_enabled && !desc.fragment_shader_msl.empty()) {
            MetalFunctionCache::instance().prefetch(
                mtl_device, desc.fragment_shader_msl, desc.fragment_entry_point,
                desc.fragment_math_mode, false);
        }
        id<MTLFunction> vertex_function =
            compile(linked_vertex_msl, desc.vertex_entry_point,
                    desc.vertex_math_mode, desc.preserve_invariance);
        id<MTLFunction> fragment_function =
            compile(desc.fragment_shader_msl, desc.fragment_entry_point,
                    desc.fragment_math_mode, false);
        if (vertex_function == nil ||
            (!desc.fragment_shader_msl.empty() &&
             fragment_function == nil)) {
            return;
        }

        // Strides from the draw (BackendPipelineDesc::dynamic_vertex_strides):
        // MTLBufferLayoutStrideDynamic, macOS 14 (Metal, MTLVertexDescriptor.h).
        NSUInteger dynamic_stride = 0;
        if (desc.dynamic_vertex_strides) {
            if (@available(macOS 14.0, *)) {
                dynamic_stride = MTLBufferLayoutStrideDynamic;
            } else {
                return;
            }
        }
        MTLVertexDescriptor* layout = [MTLVertexDescriptor vertexDescriptor];
        std::array<bool, BACKEND_MAX_VERTEX_BUFFERS> layout_used{};
        for (const BackendVertexAttribute& attribute :
             desc.vertex_attributes) {
            const unsigned slot = attribute.buffer_index;
            if (!vertex_stream_bindable(slot)) {
                static std::atomic<unsigned> logged{0};
                if (logged.fetch_add(1, std::memory_order_relaxed) == 0) {
                    std::fprintf(stderr,
                                 "[vitreo] UNIMPLEMENTED metal!newRenderPipeline"
                                 "State: input stream %u is past Metal's "
                                 "buffer argument table\n",
                                 slot);
                }
                return;
            }
            const NSUInteger mtl_index = vertex_stream_buffer_index(slot);
            layout.attributes[attribute.shader_location].format =
                metal_vertex_format(attribute.format);
            layout.attributes[attribute.shader_location].offset =
                attribute.offset;
            layout.attributes[attribute.shader_location].bufferIndex =
                mtl_index;
            layout_used[slot] = true;
        }
        for (unsigned slot = 0; slot < layout_used.size(); ++slot) {
            if (!layout_used[slot]) continue;
            const NSUInteger mtl_index = vertex_stream_buffer_index(slot);
            unsigned stride = desc.vertex_buffer_strides[slot];
            if (stride == 0 && slot == 0) stride = desc.vertex_stride;
            if (stride == 0 && dynamic_stride == 0) continue;
            layout.layouts[mtl_index].stride =
                dynamic_stride != 0 ? dynamic_stride : stride;
            if (desc.vertex_buffer_constant[slot] && dynamic_stride == 0) {
                // D3D stride 0: fetched once at the binding offset for
                // every vertex and instance, never offset by the base
                // instance.
                layout.layouts[mtl_index].stepFunction =
                    MTLVertexStepFunctionConstant;
                layout.layouts[mtl_index].stepRate = 0;
            } else if (desc.vertex_buffer_per_instance[slot]) {
                // Metal fetches element baseInstance + i / stepRate, the
                // D3D11.3 8.3.1 rule for a non-zero rate. A rate of 0
                // never advances past StartInstanceLocation: the largest
                // rate is the same fetch for every instance a draw can
                // name, where Constant would drop the base instance.
                const unsigned rate = desc.vertex_buffer_step_rates[slot];
                layout.layouts[mtl_index].stepFunction =
                    MTLVertexStepFunctionPerInstance;
                layout.layouts[mtl_index].stepRate =
                    rate != 0 ? rate : 0xFFFFFFFFu;
            } else {
                layout.layouts[mtl_index].stepFunction =
                    MTLVertexStepFunctionPerVertex;
                layout.layouts[mtl_index].stepRate = 1;
            }
        }

        const PixelFormat color0_format =
            desc.color_formats[0] != PixelFormat::Unknown
                ? desc.color_formats[0]
                : desc.color_format;
        const FormatInfo color = format_table(color0_format);
        const bool any_stride = [&] {
            for (unsigned slot = 0; slot < layout_used.size(); ++slot) {
                if (!layout_used[slot]) continue;
                unsigned stride = desc.vertex_buffer_strides[slot];
                if (stride == 0 && slot == 0) stride = desc.vertex_stride;
                if (stride != 0 || dynamic_stride != 0) return true;
            }
            return desc.vertex_stride != 0;
        }();
        // A pipeline that declares NO vertex attributes fetches nothing
        // from the input assembler, so it needs no stride. Learn
        // (SV_VertexID): a vertex shader may synthesise its vertices from
        // the index alone. Every fullscreen pass of a D3D11 renderer does this,
        // and so does the geometry-expansion pass-through, which reads its
        // vertices from a buffer by vertex_id. Requiring a stride here refused
        // those pipelines outright. This is the same contract as the
        // already-fixed "a draw with no vertex buffer is legal", one
        // layer up.
        const bool needs_stride = !desc.vertex_attributes.empty();
        // Metal render pipelines may be depth-only. In that case every
        // colour format is Unknown, but a valid depth attachment still
        // provides the rasterization target. A rasterization-DISABLED
        // pipeline needs neither: nothing is written, which is the point.
        const bool needs_attachment =
            desc.rasterization_enabled && !desc.attachmentless;
        if ((needs_attachment && !desc.depth_attachment &&
             color.mtl_pixel_format == 0) ||
            (needs_stride && !any_stride)) {
            static std::atomic<unsigned> skip_log{0};
            if (skip_log.fetch_add(1, std::memory_order_relaxed) < 8) {
                std::fprintf(stderr,
                             "[metal] pipeline skipped: color_fmt=%u "
                             "depth=%u any_stride=%u\n",
                             static_cast<unsigned>(color.mtl_pixel_format),
                             desc.depth_attachment ? 1u : 0u,
                             any_stride ? 1u : 0u);
            }
            return;
        }

        MTLRenderPipelineDescriptor* pipeline =
            [[MTLRenderPipelineDescriptor alloc] init];
        configure_metal_shader_validation(pipeline);
        pipeline.vertexFunction = vertex_function;
        // A rasterization-disabled pipeline runs the vertex stage only.
        // Metal requires no fragment function on one, and attaching it
        // anyway is rejected.
        pipeline.fragmentFunction =
            desc.rasterization_enabled ? fragment_function : nil;
        pipeline.rasterizationEnabled =
            desc.rasterization_enabled ? YES : NO;
        pipeline.vertexDescriptor = layout;
        const auto metal_color_write_mask = [](unsigned value) {
            MTLColorWriteMask mask = MTLColorWriteMaskNone;
            if ((value & 0x1) != 0) mask |= MTLColorWriteMaskRed;
            if ((value & 0x2) != 0) mask |= MTLColorWriteMaskGreen;
            if ((value & 0x4) != 0) mask |= MTLColorWriteMaskBlue;
            if ((value & 0x8) != 0) mask |= MTLColorWriteMaskAlpha;
            return mask;
        };
        for (unsigned slot = 0; slot < BACKEND_MAX_RENDER_TARGETS; ++slot) {
            const PixelFormat format = slot == 0
                ? color0_format : desc.color_formats[slot];
            const FormatInfo attachment = format_table(format);
            if (attachment.mtl_pixel_format == 0) continue;
            auto* target = pipeline.colorAttachments[slot];
            target.pixelFormat =
                static_cast<MTLPixelFormat>(attachment.mtl_pixel_format);
            const bool independent = desc.independent_blend_enabled;
            // D3D11.3 17.1 / Learn D3D11_FORMAT_SUPPORT_BLENDABLE: the
            // integer formats are not blendable and BlendEnable is
            // ignored on them (measured: R32_UINT with blending on
            // stores the source, d3d11_a07_om_probe `O9`). Metal instead
            // aborts the process in pipeline validation.
            const bool blendable = !is_integer_format(format);
            target.blendingEnabled = (blendable && (independent
                ? desc.blend_enabled_targets[slot]
                : desc.blend_enabled)) ? YES : NO;
            target.sourceRGBBlendFactor = metal_blend_factor(independent
                ? desc.source_blends[slot] : desc.source_blend);
            target.destinationRGBBlendFactor = metal_blend_factor(independent
                ? desc.destination_blends[slot] : desc.destination_blend);
            target.rgbBlendOperation = metal_blend_operation(independent
                ? desc.blend_operations[slot] : desc.blend_operation);
            target.sourceAlphaBlendFactor = metal_blend_factor(independent
                ? desc.source_blends_alpha[slot]
                : desc.source_blend_alpha);
            target.destinationAlphaBlendFactor = metal_blend_factor(independent
                ? desc.destination_blends_alpha[slot]
                : desc.destination_blend_alpha);
            target.alphaBlendOperation = metal_blend_operation(independent
                ? desc.blend_operations_alpha[slot]
                : desc.blend_operation_alpha);
            target.writeMask = metal_color_write_mask(independent
                ? desc.color_write_masks[slot] : desc.color_write_mask);
        }
        if (desc.depth_attachment) {
            // The pipeline must name the pass's own depth format: Metal
            // derives the depth-bias unit from it (UNORM r versus the
            // float exponent rule, D3D11.3 15.10), and a mismatch is a
            // validation error. Only a format with a stencil plane
            // declares a stencil attachment.
            const FormatInfo depth = format_table(desc.depth_format);
            const MTLPixelFormat depth_format =
                depth.is_depth && depth.mtl_pixel_format != 0
                    ? static_cast<MTLPixelFormat>(depth.mtl_pixel_format)
                    : MTLPixelFormatDepth32Float_Stencil8;
            pipeline.depthAttachmentPixelFormat = depth_format;
            if (depth_format == MTLPixelFormatDepth32Float_Stencil8 ||
                depth_format == MTLPixelFormatDepth24Unorm_Stencil8) {
                pipeline.stencilAttachmentPixelFormat = depth_format;
            }
        }
        // D3D11.3 17.18: o0.a becomes a coverage mask ANDed with the
        // pixel's. Metal reads the colour(0) output's alpha, which exists
        // only for a float attachment 0; the frontend declines the rest.
        if (desc.alpha_to_coverage) {
            pipeline.alphaToCoverageEnabled = YES;
        }
        if (desc.sample_count > 1) {
            pipeline.rasterSampleCount = desc.sample_count;
        }
        if (desc.tessellation) {
            // Factors come one half-precision record per patch, written
            // by the hull kernel; control points are read from buffers by
            // the post-tessellation function itself, never indexed.
            switch (desc.tessellation_partition) {
            case BackendTessellationPartition::Integer:
                pipeline.tessellationPartitionMode =
                    MTLTessellationPartitionModeInteger;
                break;
            case BackendTessellationPartition::Pow2:
                pipeline.tessellationPartitionMode =
                    MTLTessellationPartitionModePow2;
                break;
            case BackendTessellationPartition::FractionalOdd:
                pipeline.tessellationPartitionMode =
                    MTLTessellationPartitionModeFractionalOdd;
                break;
            case BackendTessellationPartition::FractionalEven:
                pipeline.tessellationPartitionMode =
                    MTLTessellationPartitionModeFractionalEven;
                break;
            }
            pipeline.tessellationFactorFormat =
                MTLTessellationFactorFormatHalf;
            pipeline.tessellationFactorStepFunction =
                MTLTessellationFactorStepFunctionPerPatch;
            pipeline.tessellationControlPointIndexType =
                MTLTessellationControlPointIndexTypeNone;
            // D3D's triangle_cw keeps the patch's own screen winding (a
            // clockwise patch rasterizes front-facing under the default
            // FrontCounterClockwise FALSE), triangle_ccw reverses it
            // (Windows d3d11_tess_winding_probe, tri and quad, every
            // partitioning). Metal's tessellator orders its domain the
            // other way round -- its points land where WARP's do, but a
            // pipeline declaring MTLWindingClockwise for triangle_cw drew
            // every such patch back-facing (culled under CULL_BACK) -- so
            // D3D's clockwise output is Metal's counter-clockwise.
            pipeline.tessellationOutputWindingOrder =
                desc.tessellation_clockwise ? MTLWindingCounterClockwise
                                            : MTLWindingClockwise;
            pipeline.tessellationFactorScaleEnabled = NO;
            pipeline.maxTessellationFactor =
                std::clamp<unsigned>(desc.tessellation_max_factor, 2u, 64u);
        }
        switch (desc.input_primitive_class) {
        case BackendPrimitiveClass::Point:
            pipeline.inputPrimitiveTopology =
                MTLPrimitiveTopologyClassPoint;
            break;
        case BackendPrimitiveClass::Line:
            pipeline.inputPrimitiveTopology =
                MTLPrimitiveTopologyClassLine;
            break;
        case BackendPrimitiveClass::Triangle:
            pipeline.inputPrimitiveTopology =
                MTLPrimitiveTopologyClassTriangle;
            break;
        case BackendPrimitiveClass::Unspecified:
            break;
        }

        pipeline.label = @((shader_tag("vs", desc.vertex_shader_msl) + "/" +
                            shader_tag("fs", desc.fragment_shader_msl) +
                            (desc.attachmentless ? "/no-attachment" : ""))
                               .c_str());
        NSError* error = nil;
        const auto state_t0 = std::chrono::steady_clock::now();
        id<MTLRenderPipelineState> state = nil;
        if (!desc.synchronous_compile && desc.cache_key != 0) {
            const compile_profile::Scope timed_state(compile_profile::MetalRenderState);
            bool failed = false;
            state = RenderStateCache::instance().acquire(
                desc.cache_key ^ ((source_features & compiled_features) * 0x4D494E4D41580002ull),
                mtl_device, pipeline, failed);
            if (state == nil) {
                (void)failed;
                return;
            }
        } else {
            state = SynchronousRenderStateCache::instance().acquire(
                mtl_device, pipeline, desc, &error);
        }
        MetalFunctionCache::instance().note_pipeline_state(
            static_cast<u64>(std::chrono::duration_cast<
                                 std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() -
                                 state_t0)
                                 .count()));
        MetalFunctionCache::instance().maybe_report();
        if (state == nil) {
            std::fprintf(stderr,
                         "[metal] pipeline creation failed: %s\n",
                         error != nil
                             ? [[error localizedDescription] UTF8String]
                             : "unknown error");
            static std::atomic<bool> dumped_failed_pipeline{false};
            if (std::getenv("VITREO_METAL_PIPELINE_DUMP") != nullptr &&
                !dumped_failed_pipeline.exchange(true)) {
                std::fprintf(stderr, "[metal] failed vertex MSL:\n%s\n",
                             linked_vertex_msl.c_str());
                std::fprintf(stderr, "[metal] failed fragment MSL:\n%s\n",
                             desc.fragment_shader_msl.c_str());
            }
            return;
        }
        report_metal_shader_validation(state);
        m_state = (__bridge_retained void*)state;
        m_dynamic_vertex_strides = dynamic_stride != 0;
        if ((source_features & ~compiled_features) != 0) {
            BackendPipelineDesc kept = desc;
            kept.vertex_shader_msl.clear();
            kept.fragment_shader_msl.clear();
            kept.synchronous_compile = true;
            kept.cache_key = 0;
            auto vertex = intern_msl_source(desc.vertex_shader_msl);
            auto fragment = intern_msl_source(desc.fragment_shader_msl);
            m_sampler_features = std::make_unique<SamplerFeatureRebuild>(
                source_features, compiled_features, [device, kept = std::move(kept), vertex, fragment]() mutable -> void* {
                    kept.vertex_shader_msl = *vertex;
                    kept.fragment_shader_msl = *fragment;
                    MetalBackendPipeline fresh(device, kept);
                    return fresh.release_state();
                });
        }
        if (m_dynamic_vertex_strides) {
            for (unsigned slot = 0; slot < layout_used.size() && slot < 32; ++slot) {
                if (layout_used[slot]) m_dynamic_stream_mask |= 1u << slot;
            }
        }
    }
}

void release_retained_metal_object(void* object) {
    if (object != nullptr) CFRelease(object);
}

std::shared_ptr<const std::string> intern_msl_source(const std::string& text) {
    static std::mutex mutex;
    static auto* table =
        new std::unordered_map<size_t, std::vector<std::weak_ptr<const std::string>>>();
    const size_t hash = std::hash<std::string>{}(text);
    std::lock_guard<std::mutex> lock(mutex);
    auto& bucket = (*table)[hash];
    for (auto it = bucket.begin(); it != bucket.end();) {
        if (auto held = it->lock()) {
            if (*held == text) return held;
            ++it;
        } else {
            it = bucket.erase(it);
        }
    }
    auto held = std::make_shared<const std::string>(text);
    bucket.push_back(held);
    return held;
}

std::unique_ptr<BackendPipeline> MetalDevice::create_pipeline(
    const BackendPipelineDesc& desc) {
    if (!available()) return nullptr;
    auto pipeline = std::make_unique<MetalBackendPipeline>(m_device, desc);
    if (!pipeline->valid()) return nullptr;
    return pipeline;
}

void MetalDevice::precompile_pipeline_functions(const BackendPipelineDesc& desc) {
    if (!available()) return;
    @autoreleasepool {
        id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
        std::string failure;
        // The keys create_pipeline looks them up by (MetalBackendPipeline):
        // the linked vertex source, the fragment source without invariance.
        const std::string vertex = linked_vertex_source(desc);
        if (!vertex.empty()) {
            (void)MetalFunctionCache::instance().get(
                device, vertex, desc.vertex_entry_point, desc.vertex_math_mode,
                desc.preserve_invariance, true, failure);
        }
        if (desc.rasterization_enabled && !desc.fragment_shader_msl.empty()) {
            failure.clear();
            (void)MetalFunctionCache::instance().get(
                device, desc.fragment_shader_msl, desc.fragment_entry_point,
                desc.fragment_math_mode, false, true, failure);
        }
    }
}

void MetalDevice::prefetch_shader(const std::string& msl,
                                  const std::string& entry_point,
                                  BackendMathMode math_mode,
                                  bool preserve_invariance) {
    if (!available()) return;
    @autoreleasepool {
        MetalFunctionCache::instance().prefetch(
            (__bridge id<MTLDevice>)m_device, msl, entry_point, math_mode,
            preserve_invariance);
    }
}

} // namespace vitreo::graphics
