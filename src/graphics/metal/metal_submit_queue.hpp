// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file metal_submit_queue.hpp
/// @brief Command stream: guest records, a worker encodes Metal.

#include <vitreo/common.hpp>
#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/block_recycler.hpp"

#include <array>
#include <deque>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace vitreo::graphics {

class MetalCommandRecorder;
class MetalSwapchain;
class MetalTexture;
struct MetalTileMappingBatch;

/// One resolved draw's plain state: every CsDraw field but the vectors, so a
/// draw can be reset by assignment and compared field by field (the delta
/// record, delta_record.hpp). Handles are `id<MTL*>`.
struct CsDrawState {
    void* pipeline{nullptr};
    void* depth_stencil{nullptr};
    u32 stencil_reference{0};
    /// The draw can write the depth or stencil attachment (depth writes on,
    /// or a stencil test with a non-zero write mask). Conservative.
    bool writes_depth_stencil{true};
    std::array<void*, 16> vertex_buffers{};
    std::array<usize, 16> vertex_offsets{};
    std::array<u32, 16> vertex_slots{};
    u32 vertex_stream_count{0};
    void* index_buffer{nullptr};
    usize index_offset{0};
    u32 index_type{0};
    bool indexed{false};
    /// push-intern (graphics/contracts/slim_record.hpp): the push blocks as
    /// spans instead of the two vectors. While `push_pending` they are the
    /// CALLER's bytes, valid only until record_draw returns; record_draw then
    /// points them at the recording's PushArena (a block equal to the stage's
    /// previous one shares its copy), or copies them into vs_const / ps_const
    /// when the draw goes where the arena does not (a captured command list,
    /// the shared-resource path) or a block is larger than an arena chunk.
    /// The encoder binds these when set, else the vectors.
    const u8* vs_push{nullptr};
    const u8* ps_push{nullptr};
    u32 vs_push_size{0};
    u32 ps_push_size{0};
    bool push_pending{false};
    std::array<void*, 16> vs_cbuffer{};
    std::array<usize, 16> vs_cbuffer_off{};
    std::array<void*, 16> ps_cbuffer{};
    std::array<usize, 16> ps_cbuffer_off{};
    std::array<void*, 16> textures{};
    std::array<void*, 16> samplers{};
    std::array<void*, 16> vertex_textures{};
    std::array<void*, 16> vertex_samplers{};
    std::array<float, 6> viewport{};
    std::array<float, 4> blend_color{1.0f, 1.0f, 1.0f, 1.0f};
    u32 scissor_x{0};
    u32 scissor_y{0};
    u32 scissor_w{0};
    u32 scissor_h{0};
    bool scissor{false};
    u32 cull_mode{0};
    u32 fill_mode{0};
    u32 depth_clip_mode{0};
    float depth_bias{0.0f};
    float depth_bias_slope_scale{0.0f};
    float depth_bias_clamp{0.0f};
    u32 winding{0};
    u32 primitive{0};
    u32 vertex_count{0};
    u32 index_count{0};
    u32 instance_count{1};
    u32 first_vertex{0};
    u32 first_instance{0};
    i32 base_vertex{0};
    /// Non-null for a GPU-sourced draw: the counts above are unused and Metal
    /// reads the argument block from this buffer at indirect_offset.
    void* indirect_args{nullptr};
    usize indirect_offset{0};
    /// Non-null for a patch draw (drawPatches) through a tessellation
    /// pipeline; the counts above are unused.
    void* tess_factors{nullptr};
    usize tess_factors_offset{0};
    u32 patch_count{0};
    u32 patch_control_points{0};
    /// Fragment-stage u# buffer UAVs, bound at BACKEND_UAV_BUFFER_BASE + slot.
    /// Texture UAVs need no array of their own: `textures` is indexed by Metal
    /// texture index, so a u# texture is placed straight at
    /// BACKEND_UAV_TEXTURE_BASE + slot.
    std::array<void*, 8> ps_uav_buffer{};
    std::array<usize, 8> ps_uav_buffer_off{};
    /// Fragment-stage UAV counters, bound at BACKEND_UAV_COUNTER_BUFFER_BASE + k.
    std::array<void*, graphics::BACKEND_MAX_UAV_COUNTER_BINDINGS> ps_uav_counter{};
    std::array<usize, graphics::BACKEND_MAX_UAV_COUNTER_BINDINGS>
        ps_uav_counter_off{};
    /// Fragment-stage PIPELINE_STATISTICS record, bound at
    /// BACKEND_FRAGMENT_STATISTICS_BUFFER_INDEX.
    void* ps_statistics{nullptr};
    usize ps_statistics_off{0};
    /// Descriptor-table argument buffers per stage, indexed by descriptor set,
    /// bound at BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + set. When any is set the
    /// flat bindings above are unused for that stage.
    std::array<void*, 4> vs_descriptor_tables{};
    std::array<void*, 4> ps_descriptor_tables{};
};

/// One resolved draw. Handles are retained `id<MTL*>` until the worker
/// finishes the list that contains them. Uniform bytes are copied.
struct CsDraw : CsDrawState {
    /// The stages' push blocks, one fixed size (PushConstants), copied for
    /// every draw: allocated here and freed by the encode thread, so their
    /// storage is recycled (block_recycler.hpp).
    using PushBytes = std::vector<u8, RecyclingAllocator<u8, 256>>;
    PushBytes vs_const;
    PushBytes ps_const;
    /// A viewport array (BackendDrawCall::viewports): all 16 slots, disabled
    /// ones already turned into empty scissor rects. Empty for the ordinary
    /// single-viewport draw, which uses the fields above.
    std::vector<std::array<float, 6>> viewports;
    std::vector<std::array<u32, 4>> scissors;
    /// Resources reachable only through the descriptor tables (and typed
    /// buffers' storage). An argument buffer does not make what it points at
    /// resident, so the encoder has to be told.
    std::vector<void*> resident;
};

/// A hash of everything @p draw makes the encoder bind and draw: every
/// handle, offset and value, and the push blocks by their bytes (wherever
/// they are kept). Two draws with equal hashes encode alike. For the delta
/// record's verify mode and its golden tests.
[[nodiscard]] u64 cs_draw_state_hash(const CsDraw& draw);

struct CsBeginPass {
    std::array<void*, 8> colors{};
    u32 color_count{0};
    void* depth{nullptr};
    u32 depth_level{0};
    u32 depth_slice{0};
    void* resolve{nullptr};
    bool preserve{true};
    bool clear_color{false};
    std::array<float, 4> color_clear{};
    /// Per-attachment clears, independent of `clear_color` (which clears every
    /// attachment to one colour): bit i clears colors[i] to color_clears[i] on
    /// load. Set by record_begin_pass when it folds the clear-only passes
    /// recorded just before this one into it (fold_trailing_clears_locked).
    u8 color_clear_mask{0};
    std::array<std::array<float, 4>, 8> color_clears{};
    bool clear_depth{false};
    bool clear_stencil{false};
    float depth_clear{1.0f};
    std::array<u32, 8> color_levels{};
    std::array<u32, 8> color_slices{};
    /// MTLRenderPassAttachmentDescriptor.depthPlane — the W slice of a 3D
    /// texture attachment. Zero for every 2D attachment.
    std::array<u32, 8> color_depth_planes{};
    u32 render_target_array_length{1};
    bool stencil{false};
    u8 stencil_clear{0};
    /// Attachment-less pass (no colours, no depth): the rasterizer's extent
    /// and sample count (MTLRenderPassDescriptor.renderTargetWidth/Height,
    /// defaultRasterSampleCount). Zero width means a normal pass.
    u32 default_width{0};
    u32 default_height{0};
    u32 default_samples{1};
    /// BackendRenderPassDesc::center_sample_positions: every sample of the
    /// multisampled attachments at (0.5, 0.5).
    bool center_samples{false};
    /// Occlusion counting (GpuOcclusionCounter): the pass's
    /// visibilityResultBuffer (an id<MTLBuffer>, retained with the op) and
    /// the offset its draws count samples into, in
    /// MTLVisibilityResultModeCounting. `visibility_accumulate` selects
    /// MTLVisibilityResultTypeAccumulate (macOS 14): the pass adds to the
    /// value already at the offset instead of replacing it.
    void* visibility_buffer{nullptr};
    usize visibility_offset{0};
    bool visibility_accumulate{false};
};

/// One resolved compute dispatch. Handles are retained `id<MTL*>` until the
/// worker finishes the list that contains them.
struct CsCompute {
    void* pipeline{nullptr};
    // Sized by the shared contract, not by a literal: the geometry-expansion
    // window raised BACKEND_MAX_COMPUTE_BUFFERS to 26 while this stayed 22, so
    // binding gs_in/gs_out/gs_counter/gs_index wrote past the array into
    // buffer_off and textures, and the encoder never bound them.
    std::array<void*, graphics::BACKEND_MAX_COMPUTE_BUFFERS> buffers{};
    std::array<usize, graphics::BACKEND_MAX_COMPUTE_BUFFERS> buffer_off{};
    std::array<void*, 16> textures{};
    /// 0..7 are s#, 8..15 their border twins.
    std::array<void*, 16> samplers{};
    /// Inline constants at buffer index 0, when the kernel has a push block.
    std::vector<u8> constants;
    u32 gx{0};
    u32 gy{0};
    u32 gz{0};
    u32 tx{1};
    u32 ty{1};
    u32 tz{1};
    /// Non-null for a GPU-sourced dispatch: gx/gy/gz are unused and Metal reads
    /// the three threadgroup counts from this buffer at indirect_offset.
    void* indirect_args{nullptr};
    usize indirect_offset{0};
    std::array<void*, 4> descriptor_tables{};
    std::vector<void*> resident;
};

/// Written once, from Metal's completion queue, when the command buffer that
/// ends at a recorded timestamp retires.
struct MetalTimestampCell {
    std::atomic<bool> done{false};
    std::atomic<u64> gpu_ns{0};
};

/// Serial command-stream thread. The guest records ops; this thread is the
/// sole owner of `MetalCommandRecorder`. INTERVAL_ONE waits for a drawable;
/// IMMEDIATE takes a pending drawable or skips only the layer blit.
class MetalSubmitQueue {
public:
    /// Leaked: a Metal completed-handler can fire after static destruction
    /// and lock m_mu ("mutex lock failed: Invalid argument" -> abort). Inline:
    /// a draw asks for it several times (recording_generation per bind).
    static MetalSubmitQueue& instance() {
        static MetalSubmitQueue* queue = new MetalSubmitQueue();
        return *queue;
    }

    /// Passes that continued in the encoder of the pass before them, and
    /// merged passes whose draw then needed an encoder of its own (see
    /// m_open_pass_ended). Monotonic, for tests and diagnostics.
    [[nodiscard]] u64 passes_continued() const noexcept {
        return m_passes_continued.load(std::memory_order_relaxed);
    }
    [[nodiscard]] u64 passes_restarted() const noexcept {
        return m_passes_restarted.load(std::memory_order_relaxed);
    }
    /// Draws whose pass's encoder was not open when the encoder reached them
    /// -- another thread's submit (or op) came between the pass's BeginPass
    /// and the draw, so the BeginPass went into an earlier list (or an op
    /// closed its encoder) -- and which the encoder placed by reopening the
    /// pass, loading what its earlier encoder stored (reopen_pass). And
    /// draws it could not place at all: no pass with that id is open (its
    /// EndPass came first, or its BeginPass was never recorded), or Metal
    /// gave no encoder. Monotonic, for tests and diagnostics
    /// (GraphicsBackendDevice::draw_encode_counts).
    [[nodiscard]] u64 draws_reopened() const noexcept {
        return m_draws_reopened.load(std::memory_order_relaxed);
    }
    [[nodiscard]] u64 draws_dropped() const noexcept {
        return m_draws_dropped.load(std::memory_order_relaxed);
    }
    /// Clear-only colour passes folded into the pass recorded after them.
    /// Monotonic, for tests and diagnostics.
    [[nodiscard]] u64 clears_folded() const noexcept {
        return m_clears_folded.load(std::memory_order_relaxed);
    }
    /// pass-fold (slim_record.hpp): BeginPasses folded at record time into
    /// the pass recorded just before them (each one an encode-time continue
    /// that no longer needs recording), and folded passes split again before
    /// a draw that may need an encoder boundary (each re-recording one
    /// BeginPass, which the encoder then continues). So the passes that
    /// share an encoder with the pass before them number
    /// passes_continued() + passes_folded() - folds_split() whether folding
    /// is on or off. Monotonic, for tests and diagnostics. The recording
    /// side counts these as plain integers under m_mu (a shared atomic line
    /// per draw would cost the recording thread a cross-core transfer), so
    /// reading one takes the lock.
    [[nodiscard]] u64 passes_folded() {
        std::lock_guard<std::mutex> lock(m_mu);
        return m_passes_folded;
    }
    [[nodiscard]] u64 folds_split() {
        std::lock_guard<std::mutex> lock(m_mu);
        return m_folds_split;
    }
    /// BeginPass and EndPass ops the encode thread has taken off submitted
    /// lists, published once per list when it is encoded. Monotonic, for
    /// tests and diagnostics.
    [[nodiscard]] u64 begin_passes_encoded() const noexcept {
        return m_begin_passes_encoded.load(std::memory_order_relaxed);
    }
    [[nodiscard]] u64 end_passes_encoded() const noexcept {
        return m_end_passes_encoded.load(std::memory_order_relaxed);
    }
    /// push-intern: push blocks copied into a recording's arena, and blocks
    /// that shared the copy of the stage's previous, equal block. Monotonic;
    /// plain under m_mu like passes_folded().
    /// pass-continue (delta_record.hpp): EndPass ops never recorded because
    /// the next pass folded in, and owed ones recorded before another op.
    [[nodiscard]] u64 ends_elided() {
        std::lock_guard<std::mutex> lock(m_mu);
        return m_ends_elided;
    }
    [[nodiscard]] u64 ends_materialized() {
        std::lock_guard<std::mutex> lock(m_mu);
        return m_ends_materialized;
    }
    [[nodiscard]] u64 push_blocks_copied() {
        std::lock_guard<std::mutex> lock(m_mu);
        return m_push_blocks_copied;
    }
    [[nodiscard]] u64 push_blocks_shared() {
        std::lock_guard<std::mutex> lock(m_mu);
        return m_push_blocks_shared;
    }
    /// delta-record (delta_record.hpp): draws recorded as deltas, the
    /// keyframes among them, their DeltaDraw bytes, the handles they took a
    /// reference on, and draws resolved again because the recording changed
    /// under them. Monotonic; plain under m_mu like passes_folded().
    struct DeltaCounters {
        u64 draws{0};
        u64 keyframes{0};
        u64 bytes{0};
        u64 retains{0};
        u64 retries{0};
        u64 runs{0};
    };
    [[nodiscard]] DeltaCounters delta_counters() {
        std::lock_guard<std::mutex> lock(m_mu);
        return m_delta;
    }
    /// The calling thread's draw to resolve its next delta draw into, reset
    /// to an unbound draw (every handle null, every count zero).
    [[nodiscard]] static CsDraw& delta_scratch();
    /// Record the calling thread's scratch draw (delta_scratch()) in pass
    /// @p pass_id as the fields that differ from the draw recorded before it
    /// in this stream -- every field at a keyframe point. Its push blocks are
    /// the caller's bytes (push_pending), interned here. @p gen is the
    /// recording generation its bind stamps were taken for: returns false,
    /// recording nothing, when the recording changed since; the caller then
    /// resolves the draw again. Only while appends_in_place().
    [[nodiscard]] bool record_draw_delta(u64 gen, u64 pass_id);
    /// Whether a draw recorded now may go through record_draw_delta.
    [[nodiscard]] bool delta_eligible() const noexcept {
        return appends_in_place();
    }
    /// Tests only: the encode thread appends cs_draw_state_hash() of every
    /// draw it encodes, full or delta, to @p sink (nullptr: stop). Change it
    /// only while the queue is drained.
    static void set_encode_hash_sink_for_test(std::vector<u64>* sink) noexcept;
    /// Tests only: a deliberate defect, for the golden test's RED arm.
    enum class DeltaMutation : u8 { None, NoRecordingKeyframe, NoChainKeyframe };
    static void set_delta_mutation_for_test(DeltaMutation mutation) noexcept;
    /// Tests only: what an append hook is told about the op just recorded.
    enum class TestAppendKind : u8 { Other, DownloadTexture, CopyBuffer };
    using AppendHook = void (*)(TestAppendKind kind, void* context);
    /// Tests only: called on the recording thread just after an op recorded
    /// through append() has joined the immediate stream (m_mu released), so
    /// a test can make another thread submit between an op's record and
    /// whatever its recorder does next -- the window a completion generation
    /// read after recording falls into. nullptr: none. Change it only while
    /// no other thread records.
    static void set_append_hook_for_test(AppendHook hook,
                                         void* context) noexcept;
    /// Tests only: VITREO_METAL_PASS_MERGE as if set to @p merge
    /// (std::nullopt: the environment again). Change it only while the queue
    /// is drained: the encoder reads it per op.
    static void set_pass_merge_for_test(std::optional<bool> merge) noexcept;

    /// Block until the previously submitted list has been encoded and the
    /// GPU has finished the back-buffer → staging copy. Safe to draw into
    /// the same back buffer afterward. Does not wait for a drawable.
    void wait_encoded();

    /// Record a new render pass. Returns the pass id that Draw / EndPass
    /// for this pass must carry so a recycled encoder cannot accept a
    /// draw that belonged to an already-ended pass.
    u64 record_begin_pass(CsBeginPass pass);
    void record_draw(CsDraw&& draw, u64 pass_id);
    void record_end_pass(u64 pass_id);
    void record_resolve(void* src, void* dst, u32 src_level = 0,
                        u32 src_slice = 0, u32 dst_level = 0,
                        u32 dst_slice = 0);
    void record_blit_region(void* src, u32 src_x, u32 src_y, void* dst,
                            u32 dst_x, u32 dst_y, u32 width, u32 height,
                            u32 src_level = 0, u32 src_slice = 0,
                            u32 dst_level = 0, u32 dst_slice = 0,
                            u32 depth = 1, u32 src_z = 0, u32 dst_z = 0);
    /// Blit a box of `texture` into `buffer` (both retained until the op has
    /// executed), in order with the work recorded around it. With
    /// record_upload_texture it moves texels between textures whose pixel
    /// formats differ but whose bytes are the same (D3D copies within a
    /// format family, block-compressed <-> uncompressed reinterpretation),
    /// or between overlapping regions of one texture.
    void record_download_texture(void* texture, u32 level, u32 slice, u32 x,
                                 u32 y, u32 z, u32 width, u32 height,
                                 u32 depth, void* buffer, usize bytes_per_row,
                                 usize bytes_per_image,
                                 usize buffer_offset = 0, u32 options = 0);
    /// Blit `staging` (an MTLBuffer the queue retains until the op has
    /// executed) into `texture` at the given level/slice/origin, in order
    /// with the draws and dispatches recorded around it.
    void record_upload_texture(void* staging, void* texture,
                               usize bytes_per_row, usize bytes_per_image,
                               u32 width, u32 height, u32 depth,
                               u32 level, u32 slice, u32 x, u32 y, u32 z,
                               u32 options = 0, usize staging_offset = 0);
    void record_copy_buffer(void* src, void* dst, usize size,
                            usize src_offset = 0, usize dst_offset = 0);
    /// Every byte of [offset, offset + size) of @p dst set to @p value.
    void record_fill_buffer(void* dst, usize offset, usize size, u8 value);
    void record_generate_mips(void* texture);
    void record_compute(CsCompute&& compute);
    /// Record a timeline signal or wait, in order with the draws around it.
    /// `event` is an id<MTLSharedEvent>; the queue retains it until the op
    /// retires, so a fence released by the guest mid-frame stays alive.
    // -- Command lists ------------------------------------------------------
    //
    // `Op` is private, so the queue owns the recordings and hands out ids. The
    // backend's BackendCommandList is a thin shell over one of these.
    using CommandListId = u64;

    /// A new, empty recording. Zero is never a valid id.
    [[nodiscard]] CommandListId create_command_list();
    void destroy_command_list(CommandListId id);
    /// Direct THIS THREAD's recorded operations into @p id. One capture per
    /// thread; nesting is refused.
    [[nodiscard]] bool begin_capture(CommandListId id);
    [[nodiscard]] bool end_capture(CommandListId id);
    /// Append a copy of @p id's operations to the recording stream, retaining
    /// every handle again so the list remains replayable.
    [[nodiscard]] bool submit_command_list(CommandListId id);
    [[nodiscard]] usize command_list_size(CommandListId id);
    void reset_command_list(CommandListId id);

    void record_signal_fence(void* event, u64 value);
    /// Record a timestamp: when encoded, the command buffer is split here and
    /// @p cell receives that buffer's GPUEndTime once the GPU retires it.
    void record_timestamp(std::shared_ptr<MetalTimestampCell> cell);
    void record_wait_fence(void* event, u64 value);
    /// What a debug-marker op does (GraphicsBackendDevice::push_debug_group
    /// and friends).
    enum class DebugMarker : u8 { Push, Pop, Signpost };
    /// Record a debug group push / pop or a signpost in command order; the
    /// worker applies it to the encoder open at that point
    /// (MetalCommandRecorder::push_debug_group).
    void record_debug_marker(DebugMarker kind, std::string label);
    /// Tiled resources (metal_tiled.hpp): a tile-mapping update, run on the
    /// Metal 4 queue between the work recorded before it and after it.
    void record_tile_mapping(std::shared_ptr<MetalTileMappingBatch> batch);
    /// Everything recorded before completes before anything after starts
    /// (ID3D11DeviceContext2::TiledResourceBarrier): tiled resources that
    /// share pool memory are distinct Metal resources, so hazard tracking
    /// does not order them.
    void record_tiled_barrier();
    /// Drop a destroyed swapchain's present staging. Must be called from the
    /// swapchain's destructor: the staging pair is keyed by chain pointer, so
    /// without this the textures outlive the surface and a later chain could
    /// be allocated at the same address and inherit them.
    void forget_swapchain(const MetalSwapchain* swapchain);

    /// @p sync_interval is the DXGI / D3D9 presentation interval: 0 is
    /// immediate, n > 0 keeps the frame up for at least n refreshes.
    void record_present(MetalSwapchain* swapchain, void* src_texture,
                        u32 width, u32 height, u32 sync_interval,
                        u32 src_x = 0, u32 src_y = 0);

    /// Hand the current recording to the worker. One list pending/encoding.
    ///
    /// A flush does not wait for the GPU (Learn, ID3D11DeviceContext::Flush:
    /// "Flush operates asynchronously"); only the worker handoff and a deep
    /// cap on GPU-incomplete lists (kMaxListsInFlight, a memory bound) hold
    /// it. A list carrying a Present is the frame throttle: it waits until
    /// the present kPresentLatency frames back has retired, which is also
    /// what bounds the renaming memory the DISCARD pool needs.
    [[nodiscard]] u64 submit();

    /// True when submitting a Present would wait right now: what
    /// DXGI_PRESENT_DO_NOT_WAIT turns into DXGI_ERROR_WAS_STILL_DRAWING.
    [[nodiscard]] bool submit_would_block();

    /// Wait until the GPU has retired @p generation. This is the completion
    /// primitive for one submitted command list; later lists do not extend
    /// the wait.
    [[nodiscard]] bool wait_for_generation(u64 generation);

    /// Submit any recording, then wait until it is GPU-complete and any
    /// present has finished. Safe for readback and teardown.
    void drain();
    /// Generation the next submitted list will carry. Work recorded now
    /// belongs to this generation, so a resource bound now is live until
    /// completed_generation() reaches it.
    [[nodiscard]] u64 recording_generation();
    /// The generation the calling thread's most recent operation went into
    /// on the immediate stream, taken under m_mu as the op joined the
    /// recording. Never one before the op's own: a recording_generation()
    /// read before recording names the open recording, which another thread
    /// can submit before the op joins it, and the op then lands in the next
    /// one -- this reports that one. And always one that is submitted when
    /// the recording is flushed, since it holds the op. A completion that
    /// must cover work this thread just recorded (a query's count, a
    /// statistics record) waits for this. 0: the op was captured into a
    /// command list (its generation is not known until it is executed), or
    /// this thread has recorded nothing. An EndPass owed by pass-continue
    /// records nothing and leaves it unchanged.
    [[nodiscard]] static u64 last_recorded_generation() noexcept {
        return t_recorded_generation;
    }

    /// Highest generation the GPU has finished. Anything at or below this is
    /// retired and its storage may be reused.
    [[nodiscard]] u64 completed_generation();
    /// True while THIS thread directs its recorded operations into a
    /// command list rather than the immediate stream.
    [[nodiscard]] static bool thread_capturing() noexcept {
        return t_capture != nullptr;
    }
    void shutdown();

    MetalSubmitQueue(const MetalSubmitQueue&) = delete;
    MetalSubmitQueue& operator=(const MetalSubmitQueue&) = delete;

private:
    MetalSubmitQueue();
    ~MetalSubmitQueue();

    enum class OpKind : u8 {
        BeginPass,
        Draw,
        /// delta-record (delta_record.hpp): one or more consecutive draws of
        /// one pass, each a DeltaDraw in the recording's delta arena
        /// (run / run_bytes / run_draws), applied in order to the encode
        /// thread's draw state.
        DrawRun,
        EndPass,
        Resolve,
        BlitRegion,
        CopyBuffer,
        /// One byte value over blit_size bytes of blit_dst at blit_dst_offset
        /// (fillBuffer:range:value:), in order with the work around it.
        FillBuffer,
        /// A CPU upload staged in a Shared buffer and blitted into a Private
        /// texture ON THE RECORDED TIMELINE, so it lands after the draws
        /// recorded before it (UpdateSubresource ordering).
        UploadTexture,
        /// The inverse of UploadTexture: a texture box into a buffer.
        DownloadTexture,
        GenerateMips,
        Compute,
        Present,
        SignalFence,
        WaitFence,
        Timestamp,
        /// A debug group push / pop or signpost (debug_marker, debug_label).
        DebugMarker,
        /// Tiled resources: a mapping update and a full barrier.
        TileMapping,
        TiledBarrier,
        Exit,
    };

    struct Op {
        /// User-provided on purpose. With an implicit default constructor,
        /// `emplace_back()` and `Op op{}` VALUE-initialise, which zero-fills
        /// the whole object -- the payload optionals' ~3 KB of storage
        /// included -- before the member initialisers run. Every member below
        /// has its own initialiser, so this skips only that memset.
        Op() noexcept {}
        OpKind kind{OpKind::EndPass};
        /// SignalFence / WaitFence: the event and the value.
        void* fence_event{nullptr};
        u64 fence_value{0};
        /// Timestamp only.
        std::shared_ptr<MetalTimestampCell> timestamp;
        /// DebugMarker only.
        DebugMarker debug_marker{DebugMarker::Signpost};
        std::string debug_label;
        /// TileMapping only.
        std::shared_ptr<MetalTileMappingBatch> tiles;
        u64 pass_id{0};
        /// Engaged only for BeginPass / Draw / Compute. Held by value but
        /// constructed only for the op's own kind: as plain members all three
        /// (~3 KB together) were value-initialised for every op recorded,
        /// which was ~4% of the render thread as memset alone.
        std::optional<CsBeginPass> begin;
        std::optional<CsDraw> draw;
        std::optional<CsCompute> compute;
        /// DrawRun only: the run's DeltaDraws, back to back.
        const u8* run{nullptr};
        u32 run_bytes{0};
        u32 run_draws{0};
        void* blit_src{nullptr};
        void* blit_dst{nullptr};
        usize blit_size{0};
        /// CopyBuffer only: byte offsets into each buffer. A whole-buffer copy
        /// leaves both at zero.
        usize blit_src_offset{0};
        usize blit_dst_offset{0};
        /// FillBuffer only: the byte written.
        u8 fill_value{0};
        u32 blit_src_x{0};
        u32 blit_src_y{0};
        u32 blit_dst_x{0};
        u32 blit_dst_y{0};
        u32 blit_src_level{0};
        u32 blit_src_slice{0};
        u32 blit_dst_level{0};
        u32 blit_dst_slice{0};
        /// UploadTexture only: staging layout and the destination box.
        usize upload_bytes_per_row{0};
        usize upload_bytes_per_image{0};
        u32 upload_width{0};
        u32 upload_height{0};
        u32 upload_depth{0};
        u32 upload_dst_z{0};
        /// UploadTexture: MTLBlitOption (the plane of a depth+stencil).
        u32 upload_options{0};
        /// BlitRegion: W extent (1 for 2D textures, the level depth for a
        /// volume copy) and the W origin on each side.
        u32 blit_depth{1};
        u32 blit_src_z{0};
        u32 blit_dst_z{0};
        MetalSwapchain* swapchain{nullptr};
        void* present_src{nullptr};
        u32 present_src_x{0};   ///< source origin of the presented rectangle
        u32 present_src_y{0};
        u32 width{0};
        u32 height{0};
        bool vsync{false};
        u32 sync_interval{0};
    };
    /// The immediate stream: recorded, submitted and encoded as one list. A
    /// deque, not a vector: an Op is ~3.4 KB and a frame's lists run to
    /// thousands of them, so a vector regrowing from empty relocated each
    /// list about twice over (5% of the render thread). A deque grows in fixed
    /// blocks, never relocates, and moves in O(1); its memory is only the ops
    /// actually in flight. Its ~54 KB blocks are recycled (block_recycler.hpp):
    /// the encode thread frees a list's blocks and the next recording reuses
    /// them, instead of a large-size-class malloc and free per block.
    using OpList = std::deque<Op, RecyclingAllocator<Op>>;

    void thread_main();
    void encode_draws(OpList& ops, u64 gpu_gen);
    bool present_from_list(OpList& ops);
    void encode_draw(const CsDraw& draw, u64 pass_id);
    /// One draw as the Draw op case encodes it: the pass-merge rules, then
    /// encode_draw. Shared by Draw and DrawRun.
    void encode_draw_in_pass(const CsDraw& draw, u64 pass_id);
    /// delta-record: copy @p bytes (one DeltaDraw) into the recording's delta
    /// arena, extending the DrawRun at the stream's tail when it is the same
    /// pass's and the bytes fit its chunk. Caller holds m_mu.
    void append_delta_locked(const u8* bytes, usize size, u64 pass_id);
    void encode_begin_pass(const CsBeginPass& pass);
    void encode_compute(const CsCompute& compute);
    void close_open_pass();
    /// Begin a new encoder for pass @p pass_id, which the encoder has met
    /// the BeginPass of and not yet the EndPass (m_live_passes), loading what
    /// its earlier encoder stored: every clear and discard of its descriptor
    /// dropped, its attachments, resolve and visibility slot kept (Reset
    /// visibility counts into a carry slot instead, see VisibilityCarry).
    /// @p fallback: the descriptor to use when the pass is not live (the
    /// open encoder's, for a pass that ended but was kept open for merging).
    /// False when there is neither or Metal gives no encoder.
    bool reopen_pass(u64 pass_id, const CsBeginPass* fallback = nullptr);
    /// m_live_passes bookkeeping, per BeginPass / EndPass op encoded, and
    /// at the end of a list (its ops are released after it).
    void note_live_begin(u64 pass_id, const CsBeginPass& begin);
    void note_live_end(u64 pass_id);
    void carry_live_passes();
    /// A submitted list's command buffers have completed: add its
    /// VisibilityCarry slots into their passes' slots, then publish @p gen.
    void list_completed(u64 gen);
    /// Whether the encoder left open by an ended pass can carry @p next as
    /// well: see m_open_pass_ended.
    [[nodiscard]] bool can_continue_pass(const CsBeginPass& next) const;
    /// Fold the clear-only colour passes at the tail of m_recording into
    /// @p next as per-attachment load clears. Caller holds m_mu.
    void fold_trailing_clears_locked(CsBeginPass& next);
    /// VITREO_METAL_PASS_BREAK_DIAG: the first can_continue_pass condition
    /// @p next fails, as a static label ("continuable" when none does).
    [[nodiscard]] const char* pass_break_reason(const CsBeginPass& next) const;
    /// VITREO_METAL_PASS_BREAK_DIAG: count why the open render encoder is
    /// about to end ahead of @p op, and report the rates every 5 s.
    void note_pass_break(const Op& op);
    [[nodiscard]] const char* attachment_read_label(const CsDraw& draw) const;
    /// Whether @p draw, encoded into an encoder that already holds earlier
    /// passes, needs an encoder boundary before it.
    [[nodiscard]] bool draw_needs_own_encoder(const CsDraw& draw) const;
    /// Selects (and if needed allocates) the staging pair for `chain`,
    /// leaving it in `m_staging_current`.
    bool ensure_staging(const MetalSwapchain* chain, void* src,
                        u32 width, u32 height);
    void flush_encoders();
    void note_encoder_closed();
    void signal_gpu_gen(u64 gen);

    void wait_until_idle_encode();
    void wait_until_present_idle();
    static void retain(void* object);
    static void release(void* object);
    static void release_draw(CsDraw& draw);
    static void release_begin(CsBeginPass& pass);
    static void release_compute(CsCompute& compute);
    static void release_op(Op& op);
    /// Symmetric to release_op, for a copy of an op that will be submitted
    /// again. Every field release_op releases must be retained here, or a second
    /// submission frees a resource the first still references.
    static void retain_op(Op& op);
    static void retain_draw(CsDraw& draw);
    static void retain_begin(CsBeginPass& pass);
    static void retain_compute(CsCompute& compute);
    /// The one place an op enters either the recording stream or a captured
    /// command list. Every record_* funnels through here, so a list captures
    /// every op kind rather than only the ones someone threaded a target through.
    void append(Op&& op);
    /// Whether a BeginPass / Draw / EndPass may be built directly in
    /// m_recording rather than through append(): see appends_in_place().
    [[nodiscard]] bool appends_in_place() const noexcept;
    /// append() for a blit op, fenced when it touches a self-aliased tiled
    /// resource (MetalTiled::self_aliased).
    void append_blit(Op&& op);
    /// Shared resources (metal_shared_resource.hpp): before an op joins the
    /// immediate stream, pull newer versions other processes published (not
    /// inside a render pass, which a blit would end), and mark the shared
    /// textures and buffers the op writes as owing a publication.
    static void note_shared_access(const Op& op);

    /// Present staging textures, PER SWAPCHAIN.
    ///
    /// This used to be a single pair shared by every swapchain, keyed on
    /// (width, height, format) and reallocated whenever a present did not
    /// match the last one. Every swapchain in the process funnels through this
    /// one queue, so with a compositing UI -- a launcher
    /// hosts ~37 child windows at sizes like 138x198, 150x264, 102x100 and
    /// 512x512 -- almost every present differed from its predecessor and
    /// allocated TWO private MTLTextures. Two GPU allocations per present,
    /// serialized on one worker.
    ///
    /// Keyed by swapchain, each surface's size is stable, so the pair is
    /// allocated once and reused. The queue stays single-threaded; what is
    /// removed is the thrash, not the ordering.
    struct PresentStaging {
        std::unique_ptr<MetalTexture> textures[2];
        u32 width{0};
        u32 height{0};
        u32 format{0};
        u32 slot{0};
    };
    std::unordered_map<const MetalSwapchain*, PresentStaging> m_staging_by_chain;
    /// The entry `ensure_staging` selected for the present being encoded.
    PresentStaging* m_staging_current{nullptr};
    std::mutex m_mu;
    std::condition_variable m_cv;
    OpList m_recording;
    /// Recorded command lists. unique_ptr keeps each vector's address stable, so
    /// a thread capturing into one appends without re-looking it up.
    std::unordered_map<CommandListId, std::unique_ptr<std::vector<Op>>>
        m_command_lists;
    CommandListId m_next_command_list{1};
    /// Where this thread's recorded operations go. Null means the immediate
    /// stream. A static member rather than a file-local because `Op` is private.
    static thread_local std::vector<Op>* t_capture;
    static thread_local CommandListId t_capture_id;
    /// last_recorded_generation(). Written under m_mu where an op joins
    /// m_recording (note_recorded_locked), 0 where one is captured.
    static thread_local u64 t_recorded_generation;
    void note_recorded_locked() noexcept {
        t_recorded_generation = m_submit_gen + 1;
    }
    /// Submitted lists the encode thread has not taken yet, oldest first,
    /// each with the generation submit() gave it. A queue rather than one
    /// slot: a D3D11 renderer flushes for query polls many times a frame, and
    /// with a single slot each flush waited for the worker to finish encoding
    /// the previous small list while the worker itself sat idle ~80% of the
    /// time.
    /// push-intern: one recording's push blocks, in fixed chunks. It belongs
    /// to the recording (m_recording_arena), moves with it into its
    /// PendingList, and goes back once the encode thread has encoded the
    /// list: the encoder copies a block where it binds it (setVertexBytes, or
    /// the recorder's constant slabs), so nothing reads the arena later. The
    /// chunks are recycled through m_free_push_chunks, under m_mu, so the
    /// recording takes a lock once a chunk rather than twice a draw.
    struct PushArena {
        static constexpr usize kChunkBytes = usize{256} << 10;
        std::vector<u8*> chunks;
        usize used{kChunkBytes};  ///< bytes used in chunks.back()
        /// The stage's (0 VS, 1 PS) previous block, for sharing.
        std::array<const u8*, 2> last{};
        std::array<u32, 2> last_size{};
        /// delta-record: the recording's DeltaDraws, in chunks of the same
        /// size (recycled with the push chunks); a DrawRun op never spans
        /// two chunks.
        std::vector<u8*> delta_chunks;
        usize delta_used{kChunkBytes};  ///< bytes used in delta_chunks.back()
        /// delta-record: handles the recording's DeltaDraws carry, one
        /// reference each, released by the encode thread once the list is
        /// encoded (outside m_mu) -- the point release_draw runs for a
        /// full draw.
        std::vector<void*> releases;
    };
    PushArena m_recording_arena;
    std::vector<u8*> m_free_push_chunks;
    static constexpr usize kMaxFreePushChunks = 64;
    /// A stable copy of @p size bytes at @p src for @p stage, in the
    /// recording's arena; null when a block cannot go there (larger than a
    /// chunk). Caller holds m_mu.
    [[nodiscard]] const u8* intern_push_locked(int stage, const u8* src,
                                               u32 size);
    /// Settle @p draw's pending push blocks into the recording's arena, or
    /// into its own vectors where that fails. Caller holds m_mu.
    void settle_push_locked(CsDraw& draw);
    /// Copy @p draw's pending push blocks into its own vectors.
    static void settle_push_into_vectors(CsDraw& draw);
    /// Give @p arena's chunks back for the next recording. Caller holds m_mu.
    void recycle_push_arena_locked(PushArena& arena) noexcept;

    /// pass-fold (slim_record.hpp), recording side, under m_mu, immediate
    /// stream only. The candidate is the pass recorded last while the stream
    /// still ends in its BeginPass, its draws and (once ended) its EndPass,
    /// with nothing else since that BeginPass. A BeginPass the encoder would
    /// continue in the candidate's encoder (can_continue_pass) reopens it:
    /// the EndPass is dropped and the candidate's id returned. Draws recorded
    /// into a reopened pass are checked against draw_needs_own_encoder's
    /// rules -- conservatively: any read of an attachment, depth included --
    /// and before one that may need a boundary the folded BeginPass is
    /// recorded again (after an EndPass), so from there the encoder sees and
    /// decides exactly the stream it would have seen without the fold. Any
    /// other op entering the stream inside a reopened pass is preceded by
    /// that same split (fold_settle_locked).
    struct FoldState {
        u64 pass_id{0};                      ///< 0: no candidate
        const CsBeginPass* begin{nullptr};   ///< its BeginPass, in m_recording
        bool ended{false};                   ///< m_recording ends in its EndPass
        bool wrote{false};                   ///< a draw since `begin` writes memory
        bool reopened{false};                ///< a pass was folded in since `begin`
        CsBeginPass replaced{};              ///< the last pass folded in
        std::array<void*, 18> roots{};       ///< attachments and their parents
        u32 root_count{0};
        /// pass-continue (delta_record.hpp): the candidate's EndPass is owed
        /// but not yet in m_recording (`ended` is set). The next pass folds
        /// into the candidate without it; anything else records it first
        /// (materialize_end_locked), so the stream is the one it would be.
        bool end_pending{false};
    };
    FoldState m_fold;
    /// pass-continue: record the candidate's owed EndPass, if any. Every
    /// path that adds to or reads the tail of m_recording calls it first,
    /// except the fold of the next pass into the candidate.
    void materialize_end_locked();
    /// pass-continue counters: EndPass ops never recorded (the next pass
    /// folded in) and recorded late.
    u64 m_ends_elided{0};
    u64 m_ends_materialized{0};
    /// delta-record, recording side, under m_mu: a token that changes
    /// whenever the stream's last recorded draw state stops being what a
    /// producer's last delta left it at -- a full Draw op joined the stream,
    /// or a command list was replayed into it -- and the producer that
    /// recorded the stream's last delta. A producer whose own chain token or
    /// identity differs records a keyframe.
    u64 m_delta_chain{1};
    const void* m_delta_producer{nullptr};
    DeltaCounters m_delta{};
    /// The delta chain breaks: the next DeltaDraw must be a keyframe.
    void break_delta_chain_locked() noexcept { ++m_delta_chain; }
    /// pass-fold and push-intern counters (see passes_folded()): plain, and
    /// written only by recording threads under m_mu, beside the state those
    /// threads already write per op.
    u64 m_passes_folded{0};
    u64 m_folds_split{0};
    u64 m_push_blocks_copied{0};
    u64 m_push_blocks_shared{0};
    u64 m_push_draws{0};
    /// Whether @p next would continue, in the encoder, the pass @p open
    /// began: can_continue_pass's descriptor rules, shared by both.
    [[nodiscard]] static bool descriptors_continue(const CsBeginPass& open,
                                                   const CsBeginPass& next);
    [[nodiscard]] bool fold_draw_needs_split_locked(const CsDraw& draw) const;
    /// Record the folded BeginPass again (after the candidate's EndPass when
    /// it has none yet), so the stream from here is the unfolded one.
    void fold_split_locked(bool end_first);
    /// Before an op other than the candidate's own draws and EndPass joins
    /// the stream: split a reopened, still open pass, and drop the candidate.
    void fold_settle_locked();
    void fold_reset_locked() noexcept;

    struct PendingList {
        OpList ops;
        u64 gen{0};
        PushArena arena;
    };
    std::deque<PendingList> m_pending;
    static constexpr usize kMaxPendingLists = 4;
    /// The recording holds a Present (set in append()).
    bool m_recording_has_present{false};
    /// Generations of the last kPresentLatency lists that carried a Present;
    /// m_present_slot names the oldest.
    static constexpr u64 kMaxListsInFlight = 16;
    static constexpr usize kPresentLatency = 2;
    std::array<u64, kPresentLatency> m_present_gens{};
    usize m_present_slot{0};
    [[nodiscard]] bool submit_blocked_locked(bool present) const noexcept;
    bool m_encode_busy{false};
    u32 m_presents_inflight{0};
    u64 m_submit_gen{0};
    u64 m_gpu_gen{0};
    std::thread m_thread;
    std::atomic<bool> m_running{true};
    /// Guest-side id of the most recently recorded BeginPass. 0 is none.
    u64 m_record_pass_id{0};

    // Cache-line groups (128-byte lines on Apple silicon). Recording threads
    // write their state per op under m_mu (above); the encode thread writes
    // its encoder state per op with no lock (the group below); and every
    // recording thread reads the generation copies per buffer bind. Each
    // group starts a line of its own, so neither thread's per-op stores
    // move a line the other is using, and a bind's read stays a hit.

    /// Lock-free copies of m_submit_gen / m_gpu_gen, stored wherever those
    /// change (under m_mu): once per submission and per completion.
    /// recording_generation() runs for every buffer a draw binds
    /// (MetalBuffer::note_gpu_bind); taking m_mu there, against the encode
    /// thread, was ~1.5% of the render thread.
    alignas(128) std::atomic<u64> m_submit_gen_published{0};
    std::atomic<u64> m_gpu_gen_published{0};

    // The encode thread's own state: nothing else touches it, except the
    // monotonic counters tests and diagnostics read.
    alignas(128) std::unique_ptr<MetalCommandRecorder> m_recorder;
    u32 m_encoders_since_commit{0};
    bool m_list_committed{false};
    /// Worker-side id of the pass whose encoder is currently open. 0 is none.
    u64 m_open_pass{0};
    /// Consecutive passes share one Metal render encoder.
    ///
    /// The D3D11 frontend records one pass per draw, and every pass loads and
    /// stores all of its attachments. On a tile-based GPU that is a full
    /// read and write of every render target per draw: at 2560x1440 with five
    /// G-buffer targets it was most of the frame's GPU time
    /// (VITREO_GPU_PROFILE: ~15,000 passes/s, one draw each). So an EndPass
    /// leaves the encoder open (m_open_pass_ended), and a following BeginPass
    /// with the same attachments and nothing to clear or resolve continues
    /// in it. Every other op already closes the open pass.
    ///
    /// Two things Metal does not order inside one encoder, while separate
    /// encoders are ordered: shader writes (UAVs, counters, statistics) from
    /// one draw against another draw's reads, and an attachment being read
    /// through a texture while it is attached. A draw that does either, in an
    /// encoder that already holds an earlier pass, gets a fresh encoder
    /// (loading what the earlier one stored), and no later pass joins an
    /// encoder that has had a writing draw. VITREO_METAL_PASS_MERGE=0 turns
    /// merging off.
    bool m_open_pass_ended{false};
    /// The open encoder holds more than one pass.
    bool m_open_pass_merged{false};
    /// A draw in the open encoder writes shader-visible memory.
    bool m_open_pass_wrote{false};
    /// The open encoder's depth/stencil attachment may differ from its
    /// texture in memory: its first pass cleared or discarded depth/stencil,
    /// or a draw since the encoder began could write them. While clear,
    /// sampling the attached depth texture reads exactly what the attachment
    /// holds (D3D11 allows that only through a read-only depth-stencil view),
    /// so such a draw needs no encoder boundary.
    bool m_open_depth_dirty{false};
    /// The descriptor the open encoder was, or would be re-, begun with.
    /// Points into the op list being encoded; reset with the encoder.
    const CsBeginPass* m_open_begin{nullptr};
    /// delta-record: the full draw state the DrawRuns of the list being
    /// encoded have built so far; invalid until the list's first keyframe and
    /// after any full Draw op.
    CsDraw m_delta_state;
    bool m_delta_valid{false};
    /// Attachment textures of the open encoder and their parent textures,
    /// filled when a pass first joins it.
    std::array<void*, 18> m_open_attachment_roots{};
    u32 m_open_attachment_root_count{0};
    std::atomic<u64> m_passes_continued{0};
    std::atomic<u64> m_passes_restarted{0};
    /// The passes whose BeginPass the encoder has met and whose EndPass it
    /// has not: a draw of one whose encoder is not open reopens it
    /// (reopen_pass). Another thread's submit between a pass's BeginPass
    /// and its draw puts the two in different lists, and an op recorded by
    /// another thread in between closes the pass's encoder; the draw used
    /// to be dropped either way. `begin` points into the list being
    /// encoded; a pass still live when its list ends keeps a retained copy
    /// in `owned` instead (carry_live_passes). Usually empty or one entry.
    struct LivePass {
        u64 pass_id{0};
        const CsBeginPass* begin{nullptr};
        std::unique_ptr<CsBeginPass> owned;
    };
    std::vector<LivePass> m_live_passes;
    /// The retained, load-only descriptor a reopened pass's encoder was
    /// begun with: m_open_begin points at it. Released with that encoder.
    std::unique_ptr<CsBeginPass> m_open_begin_owned;
    /// Reset visibility (macOS 13 has no MTLVisibilityResultTypeAccumulate:
    /// an encoder REPLACES the value at each offset it counts into) cannot
    /// continue a pass's count in a second encoder at the same offset. A
    /// reopened counting pass counts into a carry slot of its own, which is
    /// added into the pass's slot when its list's command buffers have
    /// completed -- before the list's generation is published, so a reader
    /// waiting for it sees the sum.
    struct VisibilityCarry {
        void* carry{nullptr};   ///< id<MTLBuffer>, 8 bytes Shared, retained
        void* target{nullptr};  ///< the pass's visibility buffer, retained
        usize offset{0};        ///< the pass's slot in `target`
    };
    /// The list being encoded's carries (encode thread only).
    std::vector<VisibilityCarry> m_list_carries;
    /// Handed to list_completed, by generation.
    std::mutex m_carry_mu;
    std::vector<std::pair<u64, std::vector<VisibilityCarry>>> m_carries;
    std::atomic<u32> m_carry_lists{0};
    std::atomic<u64> m_draws_reopened{0};
    std::atomic<u64> m_draws_dropped{0};
    /// Counted per list in encode_draws, published once at its end.
    std::atomic<u64> m_begin_passes_encoded{0};
    std::atomic<u64> m_end_passes_encoded{0};

    /// Written by recording threads under m_mu, once per folded clear: kept
    /// off the encode thread's lines.
    alignas(128) std::atomic<u64> m_clears_folded{0};
};

} // namespace vitreo::graphics
