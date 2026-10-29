#import <Metal/Metal.h>

#include "graphics/metal/metal_gpu_profile.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vitreo::graphics::gpu_profile {

namespace {

using Clock = std::chrono::steady_clock;

enum class Kind : u8 { Render, Compute, Blit };

/// Pipelines tracked per pass; binds past this are charged to the last slot.
constexpr usize kMaxPipelinesPerPass = 32;

struct PipelineUse {
    std::string label;
    u32 work{0};
};

struct Pass {
    Kind kind{Kind::Render};
    /// First sample index: render passes use 4 (vertex start/end, fragment
    /// start/end), compute and blit encoders 2 (start/end).
    u32 first_sample{0};
    std::string attachments;  // render: size, formats, load/store actions
    u32 work{0};
    std::vector<PipelineUse> pipelines;
    usize current{0};
};

}  // namespace

struct Recording {
    void* samples{nullptr};  // retained id<MTLCounterSampleBuffer>
    u32 capacity{0};
    u32 used{0};
    u32 unsampled{0};
    std::vector<Pass> passes;
    ~Recording();
};

namespace {

struct Agg {
    Kind kind{Kind::Render};
    u64 count{0};
    u64 invalid{0};
    u64 work{0};
    u64 ticks{0};
    u64 vertex_ticks{0};
    u64 fragment_ticks{0};
    std::unordered_map<std::string, u64> pipeline_work;
};

struct State {
    std::mutex mutex;
    bool initialized{false};
    bool usable{false};
    void* device{nullptr};       // id<MTLDevice>, retained
    void* counter_set{nullptr};  // id<MTLCounterSet>, retained
    u32 samples_per_buffer{0};
    std::vector<void*> pool;     // retained id<MTLCounterSampleBuffer>

    std::unordered_map<std::string, Agg> table;
    double cb_gpu_seconds{0.0};
    u64 cb_count{0};
    u64 unsampled{0};
    Clock::time_point window_start{};
    u64 window_gpu_ts{0};
};

State& state() {
    static State* s = new State();
    return *s;
}

double report_seconds() {
    static const double seconds = [] {
        const char* text = std::getenv("VITREO_GPU_PROFILE_SECONDS");
        const double v = text != nullptr ? std::atof(text) : 5.0;
        return v >= 0.5 ? v : 5.0;
    }();
    return seconds;
}

u64 gpu_timestamp(id<MTLDevice> device) {
    MTLTimestamp cpu = 0;
    MTLTimestamp gpu = 0;
    [device sampleTimestamps:&cpu gpuTimestamp:&gpu];
    return gpu;
}

/// One-time setup under the state mutex: the timestamp counter set and a
/// sample-buffer size the device accepts. Leaves `usable` false (and says
/// why, once) when stage-boundary sampling is not available.
void initialize_locked(State& s, id<MTLDevice> device) {
    if (s.initialized) return;
    s.initialized = true;
    if (![device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
        std::fprintf(stderr, "[gpu-profile] %s cannot sample counters at stage "
                             "boundaries; profiling is off\n",
                     device.name.UTF8String);
        return;
    }
    id<MTLCounterSet> timestamps = nil;
    for (id<MTLCounterSet> set in device.counterSets) {
        if ([set.name isEqualToString:MTLCommonCounterSetTimestamp]) {
            timestamps = set;
            break;
        }
    }
    if (timestamps == nil) {
        std::fprintf(stderr, "[gpu-profile] %s has no timestamp counter set; "
                             "profiling is off\n",
                     device.name.UTF8String);
        return;
    }
    for (u32 count : {4096u, 1024u, 256u}) {
        MTLCounterSampleBufferDescriptor* desc =
            [[MTLCounterSampleBufferDescriptor alloc] init];
        desc.counterSet = timestamps;
        desc.storageMode = MTLStorageModeShared;
        desc.sampleCount = count;
        NSError* error = nil;
        id<MTLCounterSampleBuffer> probe =
            [device newCounterSampleBufferWithDescriptor:desc error:&error];
        if (probe != nil) {
            s.samples_per_buffer = count;
            s.pool.push_back((__bridge_retained void*)probe);
            break;
        }
        std::fprintf(stderr, "[gpu-profile] %u-sample buffer refused: %s\n",
                     count,
                     error != nil ? error.localizedDescription.UTF8String : "?");
    }
    if (s.samples_per_buffer == 0) return;
    s.device = (__bridge_retained void*)device;
    s.counter_set = (__bridge_retained void*)timestamps;
    s.window_start = Clock::now();
    s.window_gpu_ts = gpu_timestamp(device);
    s.usable = true;
    std::fprintf(stderr,
                 "[gpu-profile] on: %s, stage-boundary timestamps, %u samples "
                 "per command buffer, report every %.1f s\n",
                 device.name.UTF8String, s.samples_per_buffer,
                 report_seconds());
}

/// Room for @p samples more samples in @p recording, allocating it (and its
/// sample buffer) on first use. Returns the first index, or -1.
long reserve(RecordingPtr& recording, void* queue, u32 samples) {
    State& s = state();
    if (!recording) {
        id<MTLDevice> device = ((__bridge id<MTLCommandQueue>)queue).device;
        void* buffer = nullptr;
        u32 capacity = 0;
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            initialize_locked(s, device);
            if (!s.usable) return -1;
            capacity = s.samples_per_buffer;
            if (!s.pool.empty()) {
                buffer = s.pool.back();
                s.pool.pop_back();
            }
        }
        if (buffer == nullptr) {
            MTLCounterSampleBufferDescriptor* desc =
                [[MTLCounterSampleBufferDescriptor alloc] init];
            desc.counterSet = (__bridge id<MTLCounterSet>)s.counter_set;
            desc.storageMode = MTLStorageModeShared;
            desc.sampleCount = capacity;
            NSError* error = nil;
            id<MTLCounterSampleBuffer> made =
                [device newCounterSampleBufferWithDescriptor:desc error:&error];
            if (made == nil) return -1;
            buffer = (__bridge_retained void*)made;
        }
        recording.reset(new Recording());
        recording->samples = buffer;
        recording->capacity = capacity;
    }
    if (recording->used + samples > recording->capacity) {
        ++recording->unsampled;
        return -1;
    }
    const u32 first = recording->used;
    recording->used += samples;
    return static_cast<long>(first);
}

char load_char(MTLLoadAction action) {
    switch (action) {
    case MTLLoadActionDontCare: return 'D';
    case MTLLoadActionLoad: return 'L';
    case MTLLoadActionClear: return 'C';
    }
    return '?';
}

char store_char(MTLStoreAction action) {
    switch (action) {
    case MTLStoreActionDontCare: return 'D';
    case MTLStoreActionStore: return 'S';
    case MTLStoreActionMultisampleResolve: return 'R';
    case MTLStoreActionStoreAndMultisampleResolve: return 'B';
    case MTLStoreActionUnknown: return 'U';
    case MTLStoreActionCustomSampleDepthStore: return 'X';
    }
    return '?';
}

/// "1920x1080 c0:115/LS c1:25/CS d:252/CD": size, then each attachment's
/// MTLPixelFormat value and load/store action letters (Load Clear DontCare;
/// Store DontCare Resolve, B = store and resolve). Formats stay numeric: the
/// enum in MTLPixelFormat.h is the table to read them against.
std::string describe_attachments(MTLRenderPassDescriptor* desc) {
    std::string out;
    char part[64];
    id<MTLTexture> sized = nil;
    for (NSUInteger i = 0; i < 8 && sized == nil; ++i) {
        sized = desc.colorAttachments[i].texture;
    }
    if (sized == nil) sized = desc.depthAttachment.texture;
    if (sized == nil) sized = desc.stencilAttachment.texture;
    if (sized != nil) {
        std::snprintf(part, sizeof(part), "%lux%lu",
                      static_cast<unsigned long>(sized.width),
                      static_cast<unsigned long>(sized.height));
        out += part;
        if (sized.sampleCount > 1) {
            std::snprintf(part, sizeof(part), " msaa%lu",
                          static_cast<unsigned long>(sized.sampleCount));
            out += part;
        }
    } else {
        std::snprintf(part, sizeof(part), "%lux%lu no-attachment",
                      static_cast<unsigned long>(desc.renderTargetWidth),
                      static_cast<unsigned long>(desc.renderTargetHeight));
        out += part;
    }
    for (NSUInteger i = 0; i < 8; ++i) {
        MTLRenderPassColorAttachmentDescriptor* a = desc.colorAttachments[i];
        if (a.texture == nil) continue;
        std::snprintf(part, sizeof(part), " c%lu:%lu/%c%c",
                      static_cast<unsigned long>(i),
                      static_cast<unsigned long>(a.texture.pixelFormat),
                      load_char(a.loadAction), store_char(a.storeAction));
        out += part;
    }
    if (desc.depthAttachment.texture != nil) {
        std::snprintf(part, sizeof(part), " d:%lu/%c%c",
                      static_cast<unsigned long>(
                          desc.depthAttachment.texture.pixelFormat),
                      load_char(desc.depthAttachment.loadAction),
                      store_char(desc.depthAttachment.storeAction));
        out += part;
    }
    if (desc.stencilAttachment.texture != nil) {
        std::snprintf(part, sizeof(part), " s:%c%c",
                      load_char(desc.stencilAttachment.loadAction),
                      store_char(desc.stencilAttachment.storeAction));
        out += part;
    }
    return out;
}

bool valid(u64 t) { return t != MTLCounterErrorValue && t != 0; }

const char* kind_name(Kind kind) {
    switch (kind) {
    case Kind::Render: return "render";
    case Kind::Compute: return "compute";
    case Kind::Blit: return "blit";
    }
    return "?";
}

/// Aggregation key: what the pass is, not when it ran.
std::string key_for(const Pass& pass) {
    const char* first =
        pass.pipelines.empty() ? "(none)" : pass.pipelines.front().label.c_str();
    switch (pass.kind) {
    case Kind::Render: return "RP " + pass.attachments + " | " + first;
    case Kind::Compute: return std::string("CS ") + first;
    case Kind::Blit: return "BLIT";
    }
    return "?";
}

void report_locked(State& s) {
    const Clock::time_point now = Clock::now();
    const double window =
        std::chrono::duration<double>(now - s.window_start).count();
    id<MTLDevice> device = (__bridge id<MTLDevice>)s.device;
    const u64 gpu_now = gpu_timestamp(device);
    // Convert GPU ticks with the rate measured over this same window rather
    // than assuming a unit: the counter's tick is not specified.
    const double ns_per_tick =
        gpu_now > s.window_gpu_ts
            ? window * 1e9 / static_cast<double>(gpu_now - s.window_gpu_ts)
            : 1.0;
    auto ms = [&](u64 ticks) {
        return static_cast<double>(ticks) * ns_per_tick / 1e6;
    };

    std::vector<std::pair<const std::string*, const Agg*>> rows;
    rows.reserve(s.table.size());
    u64 by_kind[3] = {0, 0, 0};
    u64 total = 0;
    for (const auto& [key, agg] : s.table) {
        rows.emplace_back(&key, &agg);
        by_kind[static_cast<int>(agg.kind)] += agg.ticks;
        total += agg.ticks;
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        return a.second->ticks > b.second->ticks;
    });

    std::fprintf(stderr,
                 "[gpu-profile] ===== %.2f s: command buffers %llu, GPU busy "
                 "%.0f ms/s; measured passes %.0f ms/s (render %.0f, compute "
                 "%.0f, blit %.0f); %llu encoders unmeasured; tick %.3f ns\n",
                 window, static_cast<unsigned long long>(s.cb_count),
                 s.cb_gpu_seconds * 1000.0 / window, ms(total) / window,
                 ms(by_kind[0]) / window, ms(by_kind[1]) / window,
                 ms(by_kind[2]) / window,
                 static_cast<unsigned long long>(s.unsampled), ns_per_tick);
    std::fprintf(stderr,
                 "[gpu-profile]   ms/s  share   n/s  avg_ms  vtx_ms  frag_ms  "
                 "work/n  pass\n");
    static const usize limit = [] {
        const char* text = std::getenv("VITREO_GPU_PROFILE_TOP");
        const long v = text != nullptr ? std::atol(text) : 40;
        return static_cast<usize>(v > 0 ? v : 40);
    }();
    for (usize i = 0; i < rows.size() && i < limit; ++i) {
        const Agg& agg = *rows[i].second;
        const double n = static_cast<double>(agg.count);
        std::fprintf(stderr,
                     "[gpu-profile] %6.1f %5.1f%% %5.1f %7.3f %7.3f %8.3f "
                     "%7.1f  %s%s\n",
                     ms(agg.ticks) / window,
                     total > 0 ? 100.0 * static_cast<double>(agg.ticks) /
                                     static_cast<double>(total)
                               : 0.0,
                     n / window, ms(agg.ticks) / n,
                     ms(agg.vertex_ticks) / n, ms(agg.fragment_ticks) / n,
                     static_cast<double>(agg.work) / n, rows[i].first->c_str(),
                     agg.invalid > 0 ? "  [some samples invalid]" : "");
        if (agg.pipeline_work.size() > 1) {
            std::vector<std::pair<std::string, u64>> pipes(
                agg.pipeline_work.begin(), agg.pipeline_work.end());
            std::sort(pipes.begin(), pipes.end(),
                      [](const auto& a, const auto& b) {
                          return a.second > b.second;
                      });
            std::string line;
            char part[160];
            for (usize p = 0; p < pipes.size() && p < 4; ++p) {
                std::snprintf(part, sizeof(part), "%s%s x%.1f",
                              p == 0 ? "" : ", ", pipes[p].first.c_str(),
                              static_cast<double>(pipes[p].second) / n);
                line += part;
            }
            std::fprintf(stderr, "[gpu-profile]          %zu pipelines: %s\n",
                         pipes.size(), line.c_str());
        }
    }
    std::fflush(stderr);
    s.table.clear();
    s.cb_gpu_seconds = 0.0;
    s.cb_count = 0;
    s.unsampled = 0;
    s.window_start = now;
    s.window_gpu_ts = gpu_now;
}

void fold(State& s, const Recording& recording,
          const MTLCounterResultTimestamp* ts, double cb_seconds) {
    std::lock_guard<std::mutex> lock(s.mutex);
    s.cb_gpu_seconds += cb_seconds > 0 ? cb_seconds : 0.0;
    ++s.cb_count;
    s.unsampled += recording.unsampled;
    for (const Pass& pass : recording.passes) {
        Agg& agg = s.table[key_for(pass)];
        agg.kind = pass.kind;
        ++agg.count;
        agg.work += pass.work;
        for (const PipelineUse& use : pass.pipelines) {
            agg.pipeline_work[use.label] += use.work;
        }
        if (ts == nullptr) {
            ++agg.invalid;
            continue;
        }
        const u64* t = &ts[pass.first_sample].timestamp;
        if (pass.kind == Kind::Render) {
            // Footprint: earliest stage start to latest stage end.
            u64 start = ~u64{0};
            u64 end = 0;
            for (int i : {0, 2}) {
                if (valid(t[i])) start = std::min(start, t[i]);
            }
            for (int i : {1, 3}) {
                if (valid(t[i])) end = std::max(end, t[i]);
            }
            if (start == ~u64{0} || end <= start) {
                ++agg.invalid;
                continue;
            }
            agg.ticks += end - start;
            if (valid(t[0]) && valid(t[1]) && t[1] > t[0]) {
                agg.vertex_ticks += t[1] - t[0];
            }
            if (valid(t[2]) && valid(t[3]) && t[3] > t[2]) {
                agg.fragment_ticks += t[3] - t[2];
            }
        } else {
            if (!valid(t[0]) || !valid(t[1]) || t[1] <= t[0]) {
                ++agg.invalid;
                continue;
            }
            agg.ticks += t[1] - t[0];
        }
    }
    const double window =
        std::chrono::duration<double>(Clock::now() - s.window_start).count();
    if (window >= report_seconds()) report_locked(s);
}

Pass& open_pass(Recording& recording, Kind kind, u32 first) {
    recording.passes.emplace_back();
    Pass& pass = recording.passes.back();
    pass.kind = kind;
    pass.first_sample = first;
    return pass;
}

}  // namespace

Recording::~Recording() {
    if (samples == nullptr) return;
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.pool.push_back(samples);
}

void RecordingDeleter::operator()(Recording* recording) const noexcept {
    delete recording;
}

bool enabled() noexcept {
    static const bool on = [] {
        const char* text = std::getenv("VITREO_GPU_PROFILE");
        return text != nullptr && text[0] != '\0' && text[0] != '0';
    }();
    return on;
}

void* begin_render(RecordingPtr& recording, void* queue, void* command_buffer,
                   void* render_pass_descriptor) {
    const long first = reserve(recording, queue, 4);
    if (first < 0) return nullptr;
    MTLRenderPassDescriptor* desc =
        (__bridge MTLRenderPassDescriptor*)render_pass_descriptor;
    MTLRenderPassSampleBufferAttachmentDescriptor* attach =
        desc.sampleBufferAttachments[0];
    attach.sampleBuffer = (__bridge id<MTLCounterSampleBuffer>)recording->samples;
    attach.startOfVertexSampleIndex = static_cast<NSUInteger>(first);
    attach.endOfVertexSampleIndex = static_cast<NSUInteger>(first + 1);
    attach.startOfFragmentSampleIndex = static_cast<NSUInteger>(first + 2);
    attach.endOfFragmentSampleIndex = static_cast<NSUInteger>(first + 3);
    id<MTLRenderCommandEncoder> enc =
        [(__bridge id<MTLCommandBuffer>)command_buffer
            renderCommandEncoderWithDescriptor:desc];
    // The descriptor may be the caller's and reused; leave it as it came.
    attach.sampleBuffer = nil;
    attach.startOfVertexSampleIndex = MTLCounterDontSample;
    attach.endOfVertexSampleIndex = MTLCounterDontSample;
    attach.startOfFragmentSampleIndex = MTLCounterDontSample;
    attach.endOfFragmentSampleIndex = MTLCounterDontSample;
    if (enc == nil) {
        recording->used -= 4;
        return nullptr;
    }
    Pass& pass = open_pass(*recording, Kind::Render, static_cast<u32>(first));
    pass.attachments = describe_attachments(desc);
    return (__bridge_retained void*)enc;
}

void* begin_compute(RecordingPtr& recording, void* queue,
                    void* command_buffer) {
    const long first = reserve(recording, queue, 2);
    if (first < 0) return nullptr;
    MTLComputePassDescriptor* desc =
        [MTLComputePassDescriptor computePassDescriptor];
    desc.dispatchType = MTLDispatchTypeConcurrent;
    MTLComputePassSampleBufferAttachmentDescriptor* attach =
        desc.sampleBufferAttachments[0];
    attach.sampleBuffer = (__bridge id<MTLCounterSampleBuffer>)recording->samples;
    attach.startOfEncoderSampleIndex = static_cast<NSUInteger>(first);
    attach.endOfEncoderSampleIndex = static_cast<NSUInteger>(first + 1);
    id<MTLComputeCommandEncoder> enc =
        [(__bridge id<MTLCommandBuffer>)command_buffer
            computeCommandEncoderWithDescriptor:desc];
    if (enc == nil) {
        recording->used -= 2;
        return nullptr;
    }
    open_pass(*recording, Kind::Compute, static_cast<u32>(first));
    return (__bridge_retained void*)enc;
}

void* begin_blit(RecordingPtr& recording, void* queue, void* command_buffer) {
    const long first = reserve(recording, queue, 2);
    if (first < 0) return nullptr;
    MTLBlitPassDescriptor* desc = [MTLBlitPassDescriptor blitPassDescriptor];
    MTLBlitPassSampleBufferAttachmentDescriptor* attach =
        desc.sampleBufferAttachments[0];
    attach.sampleBuffer = (__bridge id<MTLCounterSampleBuffer>)recording->samples;
    attach.startOfEncoderSampleIndex = static_cast<NSUInteger>(first);
    attach.endOfEncoderSampleIndex = static_cast<NSUInteger>(first + 1);
    id<MTLBlitCommandEncoder> enc =
        [(__bridge id<MTLCommandBuffer>)command_buffer
            blitCommandEncoderWithDescriptor:desc];
    if (enc == nil) {
        recording->used -= 2;
        return nullptr;
    }
    open_pass(*recording, Kind::Blit, static_cast<u32>(first));
    return (__bridge_retained void*)enc;
}

void note_pipeline(Recording* recording, const char* label) {
    if (recording == nullptr || recording->passes.empty()) return;
    Pass& pass = recording->passes.back();
    const char* name = label != nullptr && label[0] != '\0' ? label : "(unlabelled)";
    for (usize i = 0; i < pass.pipelines.size(); ++i) {
        if (pass.pipelines[i].label == name) {
            pass.current = i;
            return;
        }
    }
    if (pass.pipelines.size() >= kMaxPipelinesPerPass) {
        pass.current = pass.pipelines.size() - 1;
        return;
    }
    pass.pipelines.push_back(PipelineUse{name, 0});
    pass.current = pass.pipelines.size() - 1;
}

void note_work(Recording* recording) noexcept {
    if (recording == nullptr || recording->passes.empty()) return;
    Pass& pass = recording->passes.back();
    ++pass.work;
    if (!pass.pipelines.empty()) ++pass.pipelines[pass.current].work;
}

void attach(void* command_buffer, RecordingPtr recording) {
    if (!recording || command_buffer == nullptr) return;
    std::shared_ptr<Recording> shared(recording.release(), RecordingDeleter{});
    [(__bridge id<MTLCommandBuffer>)command_buffer
        addCompletedHandler:^(id<MTLCommandBuffer> done) {
          const double cb_seconds = done.GPUEndTime - done.GPUStartTime;
          NSData* data = nil;
          if (done.status == MTLCommandBufferStatusCompleted &&
              shared->used > 0) {
              data = [(__bridge id<MTLCounterSampleBuffer>)shared->samples
                  resolveCounterRange:NSMakeRange(0, shared->used)];
          }
          const auto* ts =
              data != nil && data.length >= shared->used *
                                                sizeof(MTLCounterResultTimestamp)
                  ? static_cast<const MTLCounterResultTimestamp*>(data.bytes)
                  : nullptr;
          fold(state(), *shared, ts, cb_seconds);
        }];
}

}  // namespace vitreo::graphics::gpu_profile
