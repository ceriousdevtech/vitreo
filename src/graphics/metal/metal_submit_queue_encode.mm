// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_submit_queue_encode.mm
/// @brief The list encoder: encode_draws, compute and encoder flushing.

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreFoundation/CoreFoundation.h>
#import <pthread.h>

#include <cmath>
#include "graphics/common/frame_profile.hpp"
#include "graphics/metal/metal_submit_queue.hpp"
#include "graphics/metal/metal_submit_queue_internal.hpp"
#include "graphics/metal/metal_tiled.hpp"
#include "graphics/metal/metal_shared_resource.hpp"
#include "graphics/metal/metal_command.hpp"
#include "graphics/metal/metal_gpu_profile.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/metal/metal_swapchain.hpp"
#include "graphics/metal/metal_texture.hpp"

#include <vitreo/present_cpu.hpp>
#include "graphics/contracts/delta_record.hpp"
#include "graphics/contracts/slim_record.hpp"

#include <algorithm>
#include <bit>
#include <string>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <utility>
#include <unordered_map>

namespace vitreo::graphics {

namespace {
constexpr u32 kMaxEncodersPerCommit = 256;

bool compute_batch_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("VITREO_METAL_COMPUTE_BATCH");
        return value == nullptr || value[0] != '0';
    }();
    return enabled && !gpu_profile::enabled() && !gpu_watch_split();
}

bool compute_buffer_bound(const CsCompute& compute, usize index) {
    // Effective presence, independent of source: current binding precedence
    // remains direct buffers, then inline bytes, then descriptor tables.
    if (index < compute.buffers.size() && compute.buffers[index]) return true;
    if (index == BACKEND_INLINE_CONSTANT_BUFFER_INDEX &&
        !compute.constants.empty()) return true;
    const usize base = BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE;
    return index >= base && index - base < compute.descriptor_tables.size() &&
           compute.descriptor_tables[index - base] != nullptr;
}

void note_compute_batch(bool reused) {
    static const bool diagnose = [] {
        const char* value = std::getenv("VITREO_COMPUTE_BATCH_DIAG");
        return value != nullptr && value[0] == '1';
    }();
    if (!diagnose) return;
    struct Counts { u64 dispatches{0}, reused{0}; };
    static thread_local Counts counts;
    ++counts.dispatches;
    counts.reused += reused ? 1u : 0u;
    if (counts.dispatches <= 32768 &&
        (counts.dispatches & (counts.dispatches - 1)) == 0) {
        std::fprintf(stderr, "[compute-batch] dispatches=%llu reused_dispatches=%llu encoder_starts=%llu\n",
                     static_cast<unsigned long long>(counts.dispatches),
                     static_cast<unsigned long long>(counts.reused),
                     static_cast<unsigned long long>(counts.dispatches - counts.reused));
    }
}

void note_attachment_barrier(bool scoped, bool closed, bool preserved) {
    static const bool diagnose = [] {
        const char* value = std::getenv("VITREO_ATTACHMENT_BARRIER_DIAG");
        return value != nullptr && value[0] == '1';
    }();
    if (!diagnose) return;
    struct Counts { u64 total{0}, scoped{0}, closed{0}, preserved{0}; };
    static thread_local Counts counts;
    ++counts.total;
    counts.scoped += scoped ? 1u : 0u;
    counts.closed += closed ? 1u : 0u;
    counts.preserved += preserved ? 1u : 0u;
    if (counts.total <= 32768 && (counts.total & (counts.total - 1)) == 0) {
        std::fprintf(stderr, "[attachment-barrier] total=%llu scoped=%llu closed=%llu preserved=%llu\n",
                     static_cast<unsigned long long>(counts.total),
                     static_cast<unsigned long long>(counts.scoped),
                     static_cast<unsigned long long>(counts.closed),
                     static_cast<unsigned long long>(counts.preserved));
    }
}

bool attachment_may_alias_scope(void* attachment, const void* scope_root) {
    if (attachment == nullptr) return false;
    void* root = texture_root(attachment);
    if (root == nullptr || root == scope_root) return true;
    id<MTLTexture> a = (__bridge id<MTLTexture>)root;
    id<MTLTexture> b = (__bridge id<MTLTexture>)scope_root;
    const auto uncertain = [](id<MTLTexture> texture) {
        return texture.storageMode != MTLStorageModePrivate ||
               ![texture respondsToSelector:@selector(isSparse)] || texture.isSparse ||
               texture.buffer != nil || texture.iosurface != nullptr;
    };
    if (uncertain(a) || uncertain(b)) return true;
    const u64 a_size = a.allocatedSize, b_size = b.allocatedSize;
    if (a_size == 0 || b_size == 0) return true;
    // Distinct private heaps (including an independent non-heap allocation)
    // do not share storage. A known standalone scope aliases only its views.
    if (b.heap == nil || a.heap != b.heap) return false;
    if (b.heap.type != MTLHeapTypePlacement) return true;
    const u64 a_offset = a.heapOffset, b_offset = b.heapOffset;
    if (a_offset > UINT64_MAX - a_size || b_offset > UINT64_MAX - b_size)
        return true;
    return a_offset < b_offset + b_size && b_offset < a_offset + a_size;
}
}  // namespace

bool MetalSubmitQueue::encode_compute(const CsCompute& compute,
                                     const CsCompute* previous, bool keep) {
    if (!m_recorder || compute.pipeline == nullptr) {
        if (m_recorder && previous) m_recorder->end_compute();
        return false;
    }
    if (previous == nullptr) {
        close_open_pass();
        m_recorder->ensure_begun();
        m_recorder->begin_compute();
    } else {
        // Concurrent Metal dispatches must retain immediate-context ordering,
        // including aliasing, atomics and compute-written indirect arguments.
        m_recorder->memory_barrier_compute();
        const usize slots = std::max({compute.buffers.size(),
            usize{BACKEND_INLINE_CONSTANT_BUFFER_INDEX} + 1,
            usize{BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE} + compute.descriptor_tables.size()});
        for (usize i = 0; i < slots; ++i) {
            if (compute_buffer_bound(*previous, i) && !compute_buffer_bound(compute, i))
                m_recorder->set_compute_buffer(nullptr, 0, static_cast<u32>(i));
        }
        for (u32 i = 0; i < compute.textures.size(); ++i) {
            if (previous->textures[i] && !compute.textures[i])
                m_recorder->set_compute_texture(nullptr, i);
        }
        for (u32 i = 0; i < compute.samplers.size(); ++i) {
            if (previous->samplers[i] && !compute.samplers[i])
                m_recorder->set_compute_sampler(nullptr, i);
        }
    }
    // Closing a preceding render pass can itself consume the last budget
    // unit, so apply the cap HERE, after close_open_pass, before this dispatch.
    keep = keep && m_encoders_since_commit < kMaxEncodersPerCommit - 1;
    m_recorder->set_compute_pipeline(compute.pipeline);
    for (u32 i = 0; i < compute.buffers.size(); ++i) {
        if (compute.buffers[i] == nullptr) {
            continue;
        }
        m_recorder->set_compute_buffer(compute.buffers[i],
                                       compute.buffer_off[i], i);
    }
    for (u32 i = 0; i < compute.textures.size(); ++i) {
        if (compute.textures[i] == nullptr) {
            continue;
        }
        m_recorder->set_compute_texture(compute.textures[i], i);
    }
    for (u32 i = 0; i < compute.samplers.size(); ++i) {
        if (compute.samplers[i] == nullptr) {
            continue;
        }
        m_recorder->set_compute_sampler(compute.samplers[i], i);
    }
    if (!compute.constants.empty()) {
        m_recorder->set_compute_bytes(compute.constants.data(),
                                      compute.constants.size(),
                                      BACKEND_INLINE_CONSTANT_BUFFER_INDEX);
    }
    for (unsigned set = 0; set < BACKEND_MAX_DESCRIPTOR_SETS; ++set) {
        if (compute.descriptor_tables[set] == nullptr) continue;
        m_recorder->set_compute_buffer(
            compute.descriptor_tables[set], 0,
            BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + set);
    }
    m_recorder->use_compute_resources(compute.resident, compute.resident_read_count);
    for (const MetalResidentToken& token : compute.resident_logs) {
        m_recorder->use_compute_resident_log(token);
    }
    if (compute.indirect_args != nullptr) {
        m_recorder->dispatch_indirect(compute.indirect_args,
                                      compute.indirect_offset, compute.tx,
                                      compute.ty, compute.tz);
    } else {
        m_recorder->dispatch(compute.gx, compute.gy, compute.gz, compute.tx,
                             compute.ty, compute.tz);
    }
    note_compute_batch(previous != nullptr);
    if (!keep) m_recorder->end_compute();
    // Preserve the existing 256 LOGICAL-dispatch/encoder commit budget even
    // though adjacent dispatches can now share a physical encoder.
    note_encoder_closed();
    // VITREO_GPU_WATCH_SPLIT=1: one command buffer per dispatch, so a slow or
    // stuck command buffer reported by gpu_watch names exactly one shader.
    // Diagnostic only -- it costs a commit per dispatch.
    static const bool split = std::getenv("VITREO_GPU_WATCH_SPLIT") != nullptr;
    if (split) flush_encoders();
    return keep;
}

void MetalSubmitQueue::flush_encoders() {
    if (m_recorder && m_recorder->has_command_buffer()) {
        m_recorder->commit();
        m_list_committed = true;
    }
    m_encoders_since_commit = 0;
}

void MetalSubmitQueue::note_encoder_closed() {
    ++m_encoders_since_commit;
    // Parks open thousands of encoders per frame. A single command buffer
    // that large plus waitUntilCompleted crashed the guest on scene load.
    if (m_encoders_since_commit >= kMaxEncodersPerCommit) {
        flush_encoders();
    }
}

namespace {

constexpr u32 kNoOpenPass = ~0u;
constexpr usize kMaxPendingStores = 32;

struct PendingStore {
    u32 begin{0};
    void* tex{nullptr};
    void* root{nullptr};
    u32 level{0};
    u32 slice{0};
    u32 plane{0};
    u32 color_bit{0};
    bool depth{false};
    bool stencil{false};
};

struct BoundTextures {
    std::array<void*, 16> tex{};
    std::array<void*, 16> vtex{};
    std::array<void*, 8> uav_buffer{};
    std::vector<void*> resident;
    void* stats{nullptr};
    void* vs_uav_table{nullptr};
    void* ps_uav_table{nullptr};
    bool counter_write{false};
    bool valid{false};

    /// draw_writes for the texture and buffer fields this walker tracks. A
    /// counter stays "writing" until the next keyframe: a later delta that
    /// unbinds one counter may leave another, and keeping the store is the
    /// safe side of that.
    [[nodiscard]] bool memory_write() const {
        if (counter_write || stats != nullptr || vs_uav_table != nullptr ||
            ps_uav_table != nullptr) {
            return true;
        }
        for (void* buffer : uav_buffer) {
            if (buffer != nullptr) return true;
        }
        for (u32 i = graphics::BACKEND_UAV_TEXTURE_BASE; i < tex.size(); ++i) {
            if (tex[i] != nullptr) return true;
        }
        return false;
    }

    void clear_writes() {
        uav_buffer.fill(nullptr);
        stats = nullptr;
        vs_uav_table = nullptr;
        ps_uav_table = nullptr;
        counter_write = false;
    }
};

void* texture_resource_root(void* handle) {
    if (handle == nullptr) return nullptr;
    id obj = (__bridge id)handle;
    if (![obj conformsToProtocol:@protocol(MTLTexture)]) return nullptr;
    return texture_root(handle);
}

/// Texture handles a DrawRun names. False when the bytes are not a delta
/// this walker understands: the caller then keeps every store still pending.
bool consume_draw_textures(const u8* record, u32 bytes, BoundTextures& bound) {
    if (bytes < sizeof(DeltaHead) + sizeof(DeltaParams)) return false;
    DeltaHead head{};
    std::memcpy(&head, record, sizeof(head));
    if (head.bytes < sizeof(DeltaHead) + sizeof(DeltaParams) ||
        head.bytes > bytes) {
        return false;
    }
    if ((head.flags & kDeltaKeyframe) != 0) {
        bound.tex.fill(nullptr);
        bound.vtex.fill(nullptr);
        bound.resident.clear();
        bound.clear_writes();
        bound.valid = true;
    } else if (!bound.valid) {
        return false;
    }
    const u8* p = record + sizeof(DeltaHead) + sizeof(DeltaParams);
    const u8* const end = record + head.bytes;
    for (u16 n = 0; n < head.entries; ++n) {
        if (p + sizeof(DeltaEntry) > end) return false;
        DeltaEntry entry{};
        std::memcpy(&entry, p, sizeof(entry));
        p += sizeof(entry);
        if (p + entry.bytes > end) return false;
        const u8* payload = p;
        p += entry.bytes;
        if (entry.field >= kFTex && entry.field < kFTex + 16 &&
            entry.bytes >= sizeof(void*)) {
            void* handle = nullptr;
            std::memcpy(&handle, payload, sizeof(handle));
            bound.tex[entry.field - kFTex] = handle;
        } else if (entry.field >= kFVTex && entry.field < kFVTex + 16 &&
                   entry.bytes >= sizeof(void*)) {
            void* handle = nullptr;
            std::memcpy(&handle, payload, sizeof(handle));
            bound.vtex[entry.field - kFVTex] = handle;
        } else if (entry.field == kFResident) {
            if (entry.a > 4096u ||
                static_cast<usize>(entry.a) * sizeof(void*) > entry.bytes) {
                return false;
            }
            bound.resident.resize(entry.a);
            if (entry.a != 0) {
                std::memcpy(bound.resident.data(), payload,
                            static_cast<usize>(entry.a) * sizeof(void*));
            }
        } else if (entry.field >= kFUav && entry.field < kFUav + 8 &&
                   entry.bytes >= sizeof(void*)) {
            void* handle = nullptr;
            std::memcpy(&handle, payload, sizeof(handle));
            bound.uav_buffer[entry.field - kFUav] = handle;
        } else if (entry.field >= kFCounter && entry.field < kFStats &&
                   entry.bytes >= sizeof(void*)) {
            void* handle = nullptr;
            std::memcpy(&handle, payload, sizeof(handle));
            if (handle != nullptr) bound.counter_write = true;
        } else if (entry.field == kFStats && entry.bytes >= sizeof(void*)) {
            void* handle = nullptr;
            std::memcpy(&handle, payload, sizeof(handle));
            bound.stats = handle;
        } else if (entry.field ==
                       kFVsTable + graphics::BACKEND_DESCRIPTOR_SET_UAV &&
                   entry.bytes >= sizeof(void*)) {
            void* handle = nullptr;
            std::memcpy(&handle, payload, sizeof(handle));
            bound.vs_uav_table = handle;
        } else if (entry.field ==
                       kFPsTable + graphics::BACKEND_DESCRIPTOR_SET_UAV &&
                   entry.bytes >= sizeof(void*)) {
            void* handle = nullptr;
            std::memcpy(&handle, payload, sizeof(handle));
            bound.ps_uav_table = handle;
        }
    }
    return true;
}

}  // namespace

void MetalSubmitQueue::plan_store_elision(
    const OpList& ops, std::vector<u32>& color_masks,
    std::vector<char>& depth_flags) const {
    color_masks.clear();
    depth_flags.clear();
    std::vector<PendingStore> pending;
    pending.reserve(8);
    BoundTextures bound{};
    // All handles are retained by this list while it is planned. Texture-view
    // ancestry is immutable, so classify each resource only once in this
    // invocation, including resources that are not textures. Dependency checks
    // still run for every occurrence: a later pass can add another store.
    std::unordered_map<void*, void*> resource_roots;
    u32 open = kNoOpenPass;
    bool blind = false;

    const auto keep_all = [&] {
        pending.clear();
        blind = true;
    };
    const auto drop_or_keep = [&](PendingStore& view, bool drop) {
        if (drop) {
            if (view.depth) {
                depth_flags[view.begin] = 1;
            } else {
                color_masks[view.begin] |= view.color_bit;
            }
        }
    };
    const auto forget_root = [&](void* root) {
        if (root == nullptr) return;
        for (usize i = 0; i < pending.size();) {
            // A sample needs the stored texels, including a sample of the
            // pass that is still open: that draw restarts the encoder and
            // reloads the attachment.
            if (pending[i].tex == root || pending[i].root == root) {
                pending[i] = pending.back();
                pending.pop_back();
                continue;
            }
            ++i;
        }
    };
    // A UAV or statistics write restarts the encoder and reloads every
    // attachment of the pass that is open, whether or not the write names
    // them.
    const auto keep_open_pass = [&] {
        if (open == kNoOpenPass) return;
        for (usize i = 0; i < pending.size();) {
            if (pending[i].begin == open) {
                pending[i] = pending.back();
                pending.pop_back();
                continue;
            }
            ++i;
        }
    };
    const auto note_handle = [&](void* handle) {
        if (pending.empty() || handle == nullptr) return;
        const auto [entry, inserted] =
            resource_roots.try_emplace(handle, nullptr);
        if (inserted) entry->second = texture_resource_root(handle);
        forget_root(entry->second);
    };
    const auto note_bound = [&] {
        if (blind || pending.empty()) return;
        for (void* handle : bound.tex) note_handle(handle);
        for (void* handle : bound.vtex) note_handle(handle);
        for (void* handle : bound.resident) note_handle(handle);
    };
    const auto same_view = [](const PendingStore& view, void* tex, u32 level,
                              u32 slice, u32 plane) {
        if (tex == nullptr) return false;
        if (level != view.level || slice != view.slice || plane != view.plane) {
            return false;
        }
        if (tex == view.tex) return true;
        void* const root = texture_root(tex);
        return root == view.root || root == view.tex;
    };
    const auto consume_begin = [&](const CsBeginPass& next) {
        if (blind) return;
        for (usize i = 0; i < pending.size();) {
            PendingStore& view = pending[i];
            if (view.begin == open) {
                ++i;
                continue;
            }
            bool attached = false;
            bool cleared = false;
            if (!view.depth) {
                for (u32 j = 0; j < next.color_count && j < next.colors.size();
                     ++j) {
                    if (!same_view(view, next.colors[j], next.color_levels[j],
                                   next.color_slices[j],
                                   next.color_depth_planes[j])) {
                        continue;
                    }
                    attached = true;
                    cleared = next.clear_color ||
                              ((next.color_clear_mask >> j) & 1u) != 0;
                    break;
                }
            } else if (next.depth != nullptr &&
                       same_view(view, next.depth, next.depth_level,
                                 next.depth_slice, 0)) {
                attached = true;
                cleared = next.clear_depth &&
                          (!view.stencil || next.clear_stencil);
            }
            if (!attached) {
                ++i;
                continue;
            }
            if (view.begin == open) {
                ++i;
                continue;
            }
            drop_or_keep(view, cleared);
            pending[i] = pending.back();
            pending.pop_back();
        }
    };
    const auto add_view = [&](u32 begin, void* tex, u32 level, u32 slice,
                              u32 plane, u32 color_bit, bool depth,
                              bool stencil) {
        if (blind || tex == nullptr || pending.size() >= kMaxPendingStores) {
            return;
        }
        PendingStore view{};
        view.begin = begin;
        view.tex = tex;
        view.root = texture_root(tex);
        view.level = level;
        view.slice = slice;
        view.plane = plane;
        view.color_bit = color_bit;
        view.depth = depth;
        view.stencil = stencil;
        pending.push_back(view);
    };

    for (const Op& op : ops) {
        switch (op.kind) {
        case OpKind::BeginPass: {
            const u32 index = static_cast<u32>(color_masks.size());
            color_masks.push_back(0);
            depth_flags.push_back(0);
            open = index;
            if (!op.begin) {
                keep_all();
                break;
            }
            consume_begin(*op.begin);
            if (op.begin->resolve != nullptr) break;
            const CsBeginPass& pass = *op.begin;
            for (u32 i = 0; i < pass.color_count && i < pass.colors.size();
                 ++i) {
                add_view(index, pass.colors[i], pass.color_levels[i],
                         pass.color_slices[i], pass.color_depth_planes[i],
                         1u << i, false, false);
            }
            add_view(index, pass.depth, pass.depth_level, pass.depth_slice, 0,
                     0, true, pass.stencil);
            break;
        }
        case OpKind::EndPass:
            open = kNoOpenPass;
            break;
        case OpKind::Draw:
            if (blind || !op.draw) break;
            for (void* handle : op.draw->textures) note_handle(handle);
            for (void* handle : op.draw->vertex_textures) note_handle(handle);
            for (void* handle : op.draw->resident) note_handle(handle);
            // A heap window may read any texture its heap names: keep
            // every pending store.
            if (!op.draw->resident_logs.empty()) keep_all();
            if (draw_writes(*op.draw)) keep_open_pass();
            break;
        case OpKind::DrawRun: {
            const u8* record = op.run;
            const u8* const end = op.run + op.run_bytes;
            for (u32 i = 0; i < op.run_draws && record < end; ++i) {
                if (record + sizeof(DeltaHead) > end) {
                    keep_all();
                    break;
                }
                DeltaHead head{};
                std::memcpy(&head, record, sizeof(head));
                if (head.bytes == 0 || record + head.bytes > end ||
                    !consume_draw_textures(record, head.bytes, bound)) {
                    keep_all();
                    break;
                }
                note_bound();
                if (bound.memory_write()) keep_open_pass();
                record += head.bytes;
            }
            break;
        }
        case OpKind::Compute:
            if (blind || !op.compute) break;
            for (void* handle : op.compute->textures) note_handle(handle);
            for (void* handle : op.compute->resident) note_handle(handle);
            if (!op.compute->resident_logs.empty()) keep_all();
            break;
        case OpKind::Resolve:
        case OpKind::BlitRegion:
        case OpKind::UploadTexture:
        case OpKind::DownloadTexture:
        case OpKind::GenerateMips:
            note_handle(op.blit_src);
            note_handle(op.blit_dst);
            break;
        case OpKind::Present:
            note_handle(op.present_src);
            break;
        case OpKind::CopyBuffer:
        case OpKind::FillBuffer:
        case OpKind::DebugMarker:
        case OpKind::Timestamp:
        case OpKind::SignalFence:
        case OpKind::WaitFence:
            break;
        default:
            // Tile maps and anything else can read an attachment through a
            // field this walk does not name.
            keep_all();
            break;
        }
    }
}

namespace {
class ImmediateCompletion final : public GpuCompletion {
public:
    bool wait() override {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [&] { return m_done; });
        return m_success;
    }
    void complete(bool success) {
        std::lock_guard lock(m_mutex);
        m_success = success;
        m_done = true;
        m_cv.notify_all();
    }
private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_done{false};
    bool m_success{false};
};
} // namespace

class MetalSubmitQueue::ImmediateLease final : public BackendImmediateExecution {
public:
    explicit ImmediateLease(MetalSubmitQueue& queue) : m_queue(queue) {}
    ~ImmediateLease() override {
        m_queue.end_immediate();
        m_queue.m_native_mutex.unlock();
    }
private:
    MetalSubmitQueue& m_queue;
};

bool MetalSubmitQueue::immediate_on_this_thread() const noexcept {
    return t_immediate == this;
}

bool MetalSubmitQueue::start_immediate() {
    // Submit the older shared recording first. Never wait for native ownership
    // with m_mu held: the worker's completion and dequeue paths require m_mu.
    for (;;) {
        (void)submit();
        if (pthread_main_np() != 0) {
            // The presentation worker can hold native ownership while waiting
            // for a drawable whose acquisition needs the main run loop. Pump
            // before taking either mutex and before installing this lease.
            while (!m_native_mutex.try_lock()) {
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true);
                if (!m_running.load(std::memory_order_acquire)) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } else {
            m_native_mutex.lock();
        }
        {
            std::unique_lock lock(m_mu);
            if (!m_running.load(std::memory_order_acquire)) {
                m_native_mutex.unlock();
                return false;
            }
            if (m_pending.empty() && !m_encode_busy && m_recording.empty() &&
                m_submit_gen - m_gpu_gen < kMaxListsInFlight) {
                m_immediate_gen = ++m_submit_gen;
                m_submit_gen_published.store(m_submit_gen, std::memory_order_release);
                m_immediate_active = true;
                t_immediate = this;
                t_recorded_generation = m_immediate_gen;
                break_delta_chain_locked();
                break;
            }
        }
        m_native_mutex.unlock();
        std::unique_lock lock(m_mu);
        m_cv.wait_for(lock, std::chrono::milliseconds(1));
    }
    if (!m_recorder) {
        m_recorder = std::make_unique<MetalCommandRecorder>(
            MetalDevice::instance().command_queue_ptr());
    }
    m_delta_valid = false;
    m_list_committed = false;
    return true;
}

std::unique_ptr<BackendImmediateExecution> MetalSubmitQueue::begin_immediate() {
    // A nested or captured scope cannot acquire the same native encoder twice.
    if (t_immediate != nullptr || t_capture != nullptr) return nullptr;
    if (!start_immediate()) return nullptr;
    return std::make_unique<ImmediateLease>(*this);
}

void MetalSubmitQueue::encode_immediate(Op&& op) {
    @autoreleasepool {
        const CsCompute* previous = m_immediate_compute && m_immediate_compute->compute
            ? &*m_immediate_compute->compute : nullptr;
        if (previous && op.kind != OpKind::Compute) {
            m_recorder->end_compute();
            previous = nullptr;
            release_op(*m_immediate_compute);
            m_immediate_compute.reset();
        }
        const Op* present = nullptr;
        u64 begins = 0, ends = 0;
        encode_op(op, m_immediate_gen, previous, present,
                  compute_batch_enabled(), 0, false, begins, ends);
        t_recorded_generation = m_immediate_gen;
        if (begins) m_begin_passes_encoded.fetch_add(begins, std::memory_order_relaxed);
        if (ends) m_end_passes_encoded.fetch_add(ends, std::memory_order_relaxed);
        if (op.kind == OpKind::BeginPass) {
            // The direct draw borrows its push bytes only until return, but a
            // render-pass descriptor must survive until EndPass or reopening.
            carry_live_passes();
            if (m_immediate_begin) release_op(*m_immediate_begin);
            m_immediate_begin = std::move(op);
            if (m_open_pass != 0) m_open_begin = &*m_immediate_begin->begin;
        } else if (op.kind == OpKind::Compute && previous) {
            if (m_immediate_compute) release_op(*m_immediate_compute);
            m_immediate_compute = std::move(op);
        } else {
            if (op.kind == OpKind::Compute && m_immediate_compute) {
                release_op(*m_immediate_compute);
                m_immediate_compute.reset();
            }
            release_op(op);
        }
    }
}

std::shared_ptr<GpuCompletion> MetalSubmitQueue::flush_immediate(bool retire_generation) {
    @autoreleasepool {
        if (m_immediate_compute) {
            m_recorder->end_compute();
            release_op(*m_immediate_compute);
            m_immediate_compute.reset();
        }
        close_open_pass();
        carry_live_passes();
        if (m_immediate_begin) {
            release_op(*m_immediate_begin);
            m_immediate_begin.reset();
        }
        // This token names precisely this CB, not the lease's global generation.
        // Query resolve callbacks may depend on it before the lease has ended.
        auto completion = std::make_shared<ImmediateCompletion>();
        auto carries = std::move(m_list_carries);
        m_list_carries.clear();
        m_recorder->ensure_begun();
        id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)
            m_recorder->command_buffer_for_encoders();
        [cb addCompletedHandler:^(id<MTLCommandBuffer> finished) {
            complete_visibility_carries(carries);
            completion->complete(finished.status == MTLCommandBufferStatusCompleted);
        }];
        if (retire_generation) {
            m_recorder->commit_with_handler(
                [](u64 gen) { MetalSubmitQueue::instance().list_completed(gen); },
                m_immediate_gen);
        } else {
            m_recorder->commit();
        }
        m_encoders_since_commit = 0;
        m_list_committed = true;
        return completion;
    }
}

void MetalSubmitQueue::end_immediate() {
    if (!immediate_on_this_thread()) return;
    // This CB retires the whole lease, including earlier query/tiled commits.
    // Later generations cannot encode until native ownership is released.
    (void)flush_immediate(true);
    m_recorder->recycle_constant_slabs_after_present();
    m_list_committed = false;
    t_immediate = nullptr;
    {
        std::lock_guard lock(m_mu);
        m_immediate_active = false;
        m_cv.notify_all();
    }
}

bool MetalSubmitQueue::wait_immediate(u64 generation) {
    // Synchronous readback relinquishes native ownership before a GPU wait.
    // Finalize G first; a later producer can now encode G+1 without retiring a
    // still-open lease. Reacquiring reserves a new generation in stream order.
    if (generation == 0) generation = m_immediate_gen;
    end_immediate();
    m_native_mutex.unlock();
    const bool success = wait_for_generation(generation);
    const bool resumed = start_immediate();
    // Shutdown may prevent resuming. The RAII owner still owns the mutex so its
    // destructor can release it uniformly; it has no live immediate generation.
    if (!resumed) m_native_mutex.lock();
    return success && resumed;
}

void MetalSubmitQueue::encode_op(Op& op, u64 gpu_gen,
    const CsCompute*& previous_compute, const Op*& present,
    bool keep_compute, u32 discard_colors, bool discard_depth,
    u64& begins, u64& ends) {
    const bool profile = frame_profile::enabled();
            switch (op.kind) {
            case OpKind::BeginPass: {
                ++begins;
                if (profile) frame_profile::add(frame_profile::Counter::PassesBegun);

                note_live_begin(op.pass_id, *op.begin);
                if (can_continue_pass(*op.begin)) {
                    // Same attachments, loaded: carry on in the open encoder.
                    if (!m_open_pass_merged) {
                        const CsBeginPass& open = *m_open_begin;
                        u32 n = 0;
                        const auto add = [&](void* texture) {
                            if (texture == nullptr) return;
                            m_open_attachment_roots[n++] = texture;
                            void* root = texture_root(texture);
                            if (root != texture) {
                                m_open_attachment_roots[n++] = root;
                            }
                        };
                        for (u32 i = 0;
                             i < open.color_count && i < open.colors.size();
                             ++i) {
                            add(open.colors[i]);
                        }
                        add(open.depth);
                        m_open_attachment_root_count = n;
                        m_open_pass_merged = true;
                    }
                    m_open_pass = op.pass_id;
                    m_open_pass_ended = false;
                    m_open_begin = &*op.begin;
                    m_passes_continued.fetch_add(1, std::memory_order_relaxed);
                    if (profile) {
                        frame_profile::add(frame_profile::Counter::PassesMerged);
                    }
                    if (op.begin->visibility_buffer != nullptr) {
                        m_recorder->set_visibility_counting(
                            op.begin->visibility_offset);
                    }
                    break;
                }
                if (m_open_pass != 0) {
                    close_open_pass();
                }
                {
                    if (discard_colors != 0 || discard_depth) {
                        m_stores_elided.fetch_add(
                            static_cast<u64>(std::popcount(discard_colors)) +
                                (discard_depth ? 1u : 0u),
                            std::memory_order_relaxed);
                    }
                    encode_begin_pass(*op.begin, discard_colors, discard_depth);
                }
                m_open_pass =
                    m_recorder && m_recorder->has_render_encoder() ? op.pass_id
                                                                   : 0;
                m_open_begin = m_open_pass != 0 ? &*op.begin : nullptr;
                // A cleared or discarded attachment is not what memory holds.
                m_open_depth_dirty = op.begin->clear_depth ||
                                     !op.begin->preserve;
                m_open_stencil_dirty = op.begin->clear_stencil ||
                                       !op.begin->preserve;
                m_open_color_dirty = op.begin->clear_color ||
                                     op.begin->color_clear_mask != 0 ||
                                     !op.begin->preserve;
                break;
            }
            case OpKind::Draw:
                // A full draw is state the DrawRuns after it do not know:
                // the next one starts from a keyframe (break_delta_chain).
                m_delta_valid = false;
                encode_draw_in_pass(*op.draw, op.pass_id);
                if (profile) frame_profile::add(frame_profile::Counter::EncodedDraws);
                break;
            case OpKind::DrawRun: {
                if (profile) {
                    frame_profile::add(frame_profile::Counter::EncodedDraws, op.run_draws);
                }
                const u8* record = op.run;
                const u8* const end = op.run + op.run_bytes;
                for (u32 i = 0; i < op.run_draws && record < end; ++i) {
                    const auto* head = reinterpret_cast<const DeltaHead*>(record);
                    if (!apply_delta(record, m_delta_state, m_delta_valid)) {
                        static std::atomic<bool> told{false};
                        if (!told.exchange(true)) {
                            std::fprintf(stderr,
                                         "[r3] ERROR: a delta draw with no "
                                         "keyframe before it was dropped\n");
                        }
                    } else {
                        if (delta_record::verify() &&
                            cs_draw_state_hash(m_delta_state) != head->hash) {
                            std::fprintf(stderr,
                                         "[r3] VERIFY FAILED: the applied "
                                         "delta draw state differs from the "
                                         "recorded draw (pass %llu)\n",
                                         static_cast<unsigned long long>(
                                             op.pass_id));
                            std::abort();
                        }
                        encode_draw_in_pass(m_delta_state, op.pass_id);
                    }
                    record += head->bytes;
                }
                break;
            }
            case OpKind::EndPass:
                ++ends;
                note_live_end(op.pass_id);
                if (op.pass_id != 0 && op.pass_id == m_open_pass) {
                    // VITREO_GPU_WATCH_SPLIT=1 also isolates each render
                    // pass, so a stuck buffer names one pass's draws.
                    const bool split = gpu_watch_split();
                    if (split || !pass_merge_enabled()) {
                        close_open_pass();
                        if (split) flush_encoders();
                    } else {
                        // Kept open for a following pass on the same
                        // attachments; any other op closes it.
                        m_open_pass_ended = true;
                    }
                }
                break;
            case OpKind::Resolve:
                close_open_pass();
                if (m_recorder && op.blit_src && op.blit_dst) {
                    m_recorder->resolve_msaa_texture(
                        op.blit_src, op.blit_dst, op.blit_src_level,
                        op.blit_src_slice, op.blit_dst_level,
                        op.blit_dst_slice);
                    note_encoder_closed();
                }
                break;
            case OpKind::BlitRegion:
                close_open_pass();
                if (m_recorder && op.blit_src && op.blit_dst) {
                    m_recorder->ensure_begun();
                    m_recorder->blit_copy_texture_region(
                        op.blit_src, op.blit_src_x, op.blit_src_y, op.blit_dst,
                        op.blit_dst_x, op.blit_dst_y, op.width, op.height,
                        op.blit_src_level, op.blit_src_slice,
                        op.blit_dst_level, op.blit_dst_slice, op.blit_depth,
                        op.blit_src_z, op.blit_dst_z);
                    note_encoder_closed();
                }
                break;
            case OpKind::DownloadTexture:
                close_open_pass();
                if (m_recorder && op.blit_src && op.blit_dst) {
                    m_recorder->ensure_begun();
                    m_recorder->blit_copy_texture_to_buffer(
                        op.blit_src, op.blit_src_level, op.blit_src_slice,
                        op.blit_src_x, op.blit_src_y, op.blit_src_z,
                        op.upload_width, op.upload_height, op.upload_depth,
                        op.blit_dst, op.upload_bytes_per_row,
                        op.upload_bytes_per_image, op.blit_dst_offset,
                        op.upload_options);
                    note_encoder_closed();
                }
                break;
            case OpKind::CopyBuffer:
                close_open_pass();
                if (m_recorder && op.blit_src && op.blit_dst &&
                    op.blit_size != 0) {
                    m_recorder->ensure_begun();
                    m_recorder->blit_copy_buffer(
                        op.blit_src, op.blit_src_offset, op.blit_dst,
                        op.blit_dst_offset, op.blit_size);
                    note_encoder_closed();
                }
                break;
            case OpKind::FillBuffer:
                close_open_pass();
                if (m_recorder && op.blit_dst && op.blit_size != 0) {
                    m_recorder->ensure_begun();
                    m_recorder->blit_fill_buffer(op.blit_dst,
                                                 op.blit_dst_offset,
                                                 op.blit_size, op.fill_value);
                    note_encoder_closed();
                }
                break;
            case OpKind::UploadTexture:
                close_open_pass();
                if (m_recorder && op.blit_src && op.blit_dst) {
                    m_recorder->ensure_begun();
                    m_recorder->blit_copy_buffer_to_texture(
                        op.blit_src, op.upload_bytes_per_row,
                        op.upload_bytes_per_image, op.upload_width,
                        op.upload_height, op.upload_depth, op.blit_dst,
                        op.blit_dst_level, op.blit_dst_slice, op.blit_dst_x,
                        op.blit_dst_y, op.upload_dst_z, op.upload_options,
                        op.blit_src_offset);
                    note_encoder_closed();
                }
                break;
            case OpKind::GenerateMips:
                close_open_pass();
                if (m_recorder && op.blit_src) {
                    m_recorder->ensure_begun();
                    m_recorder->blit_generate_mipmaps(op.blit_src);
                    note_encoder_closed();
                }
                break;
            case OpKind::Compute: {
                if (profile) frame_profile::add(frame_profile::Counter::EncodedDispatches);
                if (op.compute) {
                    previous_compute = encode_compute(*op.compute, previous_compute, keep_compute)
                        ? &*op.compute : nullptr;
                } else {
                    if (previous_compute) m_recorder->end_compute();
                    previous_compute = nullptr;
                }
                break;
            }
            case OpKind::Present:
                present = &op;
                break;
            case OpKind::SignalFence:
                // Close the open pass first: a signal means "everything recorded
                // before this is done", and an open pass is not done.
                // A fence op can be the first thing in a list; with no
                // command buffer begun the recorder dropped it silently.
                close_open_pass();
                m_recorder->ensure_begun();
                m_recorder->signal_event(op.fence_event, op.fence_value);
                break;
            case OpKind::WaitFence:
                close_open_pass();
                m_recorder->ensure_begun();
                m_recorder->wait_for_event(op.fence_event, op.fence_value);
                break;
            case OpKind::Timestamp:
                // A timestamp is "the GPU has finished everything before
                // this", so it ends the command buffer here and takes that
                // buffer's GPUEndTime. Command buffers on one queue retire in
                // order, and an empty one still reports a GPUEndTime after
                // its predecessors (measured), so a timestamp with no work
                // since the last split is ordered correctly too.
                close_open_pass();
                if (m_recorder && op.timestamp) {
                    m_recorder->ensure_begun();
                    m_recorder->commit_with_end_time(
                        [](void* context, double gpu_end_seconds) {
                            auto* cell = static_cast<
                                std::shared_ptr<MetalTimestampCell>*>(context);
                            (*cell)->gpu_ns.store(
                                static_cast<u64>(std::llround(
                                    gpu_end_seconds * 1e9)),
                                std::memory_order_relaxed);
                            (*cell)->done.store(true,
                                                std::memory_order_release);
                            delete cell;
                        },
                        new std::shared_ptr<MetalTimestampCell>(op.timestamp));
                    m_list_committed = true;
                    m_encoders_since_commit = 0;
                }
                break;
            case OpKind::DebugMarker:
                // Applied where it stands: inside an open pass it marks that
                // pass's encoder (MetalCommandRecorder mirrors open groups
                // onto every encoder and balances them), so a marker never
                // splits a pass.
                if (!m_recorder) break;
                switch (op.debug_marker) {
                case DebugMarker::Push:
                    m_recorder->push_debug_group(op.debug_label);
                    break;
                case DebugMarker::Pop:
                    m_recorder->pop_debug_group();
                    break;
                case DebugMarker::Signpost:
                    m_recorder->insert_debug_signpost(op.debug_label);
                    break;
                }
                break;
            case OpKind::TileMapping: {
                // The command buffer so far signals the shared event, the
                // Metal 4 queue waits for it and updates the mappings, and
                // the next command buffer waits for the update (Metal orders
                // events against command-buffer boundaries).
                close_open_pass();
                auto& tiled = MetalTiled::instance();
                if (m_recorder && op.tiles && tiled.available()) {
                    const u64 before = tiled.take_event_values();
                    m_recorder->ensure_begun();
                    m_recorder->signal_event(tiled.event(), before);
                    flush_encoders();
                    tiled.encode(op.tiles, before, before + 1, gpu_gen);
                    m_recorder->ensure_begun();
                    m_recorder->wait_for_event(tiled.event(), before + 1);
                }
                break;
            }
            case OpKind::TiledBarrier: {
                // A signal completes when everything before it has; a wait
                // holds everything after it: together, a full barrier.
                close_open_pass();
                auto& tiled = MetalTiled::instance();
                if (m_recorder && tiled.available()) {
                    const u64 value = tiled.take_event_values();
                    m_recorder->ensure_begun();
                    m_recorder->signal_event(tiled.event(), value);
                    m_recorder->wait_for_event(tiled.event(), value);
                }
                break;
            }
            case OpKind::AttachmentBarrier: {
                // The open pass stores its attachments; a later pass with
                // the same ones loads them in a new encoder. A transition
                // of an unrelated standalone texture need not store tiles.
                const void* root = texture_root(op.blit_src);
                bool needed = op.blit_src == nullptr || root == nullptr;
                if (!needed && m_open_begin) {
                    const auto matches = [&](void* attachment) {
                        return attachment_may_alias_scope(attachment, root);
                    };
                    needed = matches(m_open_begin->depth) || matches(m_open_begin->resolve);
                    for (u32 i = 0; !needed && i < m_open_begin->color_count &&
                         i < m_open_begin->colors.size(); ++i)
                        needed = matches(m_open_begin->colors[i]);
                }
                const bool open = m_open_pass != 0;
                note_attachment_barrier(op.blit_src != nullptr, needed && open, !needed && open);
                if (needed) close_open_pass();
                break;
            }
            case OpKind::Encode:
                close_open_pass();
                if (m_recorder && op.encode) {
                    m_recorder->ensure_begun();
                    op.encode(m_recorder->command_buffer_for_encoders());
                    note_encoder_closed();
                }
                break;
            case OpKind::Exit:
                break;
            }
}

void MetalSubmitQueue::encode_draws(OpList& ops, u64 gpu_gen) {
    // VITREO_D3D12_PROFILE: the worker's time for this list.
    frame_profile::Scope profile_encode(frame_profile::Time::Encode);
    const bool profile = frame_profile::enabled();
    @autoreleasepool {
        if (!m_recorder) {
            m_recorder = std::make_unique<MetalCommandRecorder>(
                MetalDevice::instance().command_queue_ptr());
        }
        m_list_committed = false;
        // delta-record: every list starts from its first keyframe.
        m_delta_valid = false;
        std::vector<u32> store_color_masks;
        std::vector<char> store_depth_flags;
        plan_store_elision(ops, store_color_masks, store_depth_flags);
        u32 store_begin = 0;
        const Op* present = nullptr;
        static const bool pass_break_diag =
            std::getenv("VITREO_METAL_PASS_BREAK_DIAG") != nullptr;
        // Counted here, published once after the list (begin_passes_encoded).
        u64 begins = 0;
        u64 ends = 0;
        const CsCompute* previous_compute = nullptr;
        const bool batch_compute = compute_batch_enabled();
        for (auto it = ops.begin(); it != ops.end(); ++it) {
            Op& op = *it;
            if (previous_compute && op.kind != OpKind::Compute) {
                m_recorder->end_compute();
                previous_compute = nullptr;
            }
            if (pass_break_diag || profile) note_pass_break(op);
            const u32 store_index = store_begin;
            if (op.kind == OpKind::BeginPass) ++store_begin;
            const auto next = std::next(it);
            const bool keep = batch_compute && next != ops.end() &&
                next->kind == OpKind::Compute && next->compute &&
                next->compute->pipeline != nullptr;
            encode_op(op, gpu_gen, previous_compute, present, keep,
                store_index < store_color_masks.size() ? store_color_masks[store_index] : 0,
                store_index < store_depth_flags.size() && store_depth_flags[store_index] != 0,
                begins, ends);
        }
        if (previous_compute) m_recorder->end_compute();
        close_open_pass();
        // The list's ops are released once it is encoded: a pass still open
        // (its EndPass is in a later list) keeps its descriptor here.
        if (!m_live_passes.empty()) [[unlikely]] carry_live_passes();
        if (!m_list_carries.empty()) [[unlikely]] {
            std::lock_guard<std::mutex> lock(m_carry_mu);
            m_carries.emplace_back(gpu_gen, std::move(m_list_carries));
            m_list_carries.clear();
            m_carry_lists.fetch_add(1, std::memory_order_release);
        }
        if (begins != 0) {
            m_begin_passes_encoded.fetch_add(begins, std::memory_order_relaxed);
        }
        if (ends != 0) {
            m_end_passes_encoded.fetch_add(ends, std::memory_order_relaxed);
        }
        if (present != nullptr && present->present_src != nullptr &&
            ensure_staging(present->swapchain, present->present_src,
                           present->width, present->height)) {
            if (present->swapchain != nullptr) {
                present->swapchain->wait_while_held(
                    &m_staging_current->held[m_staging_current->slot]);
            }
            m_recorder->ensure_begun();
            m_recorder->blit_copy_texture_region(
                present->present_src, present->present_src_x,
                present->present_src_y,
                m_staging_current->textures[m_staging_current->slot]->handle(),
                0, 0, present->width, present->height, 0, 0, 0, 0);
        }
        if (m_recorder && !m_recorder->has_command_buffer()) {
            // A generation with no newly encoded GPU commands can still
            // follow an earlier GPU-incomplete generation. Its completion
            // must be ordered on the Metal queue: advancing m_gpu_gen from
            // the CPU would make waits for the earlier generation succeed
            // before its IOSurface writes are visible in another process.
            // This also covers mid-frame flushes committed above.
            m_recorder->ensure_begun();
        }
        if (m_recorder && m_recorder->has_command_buffer()) {
            // Worker must not waitUntilCompleted here: a park-load CB is
            // huge, and blocking this thread on the GPU deadlocks Present
            // behind nextDrawable and has aborted the process.
            m_recorder->commit_with_handler(
                [](u64 gen) { MetalSubmitQueue::instance().list_completed(gen); },
                gpu_gen);
        } else {
            list_completed(gpu_gen);
        }
        m_encoders_since_commit = 0;
        m_list_committed = false;
        if (m_recorder) {
            m_recorder->recycle_constant_slabs_after_present();
        }
    }
}

} // namespace vitreo::graphics
