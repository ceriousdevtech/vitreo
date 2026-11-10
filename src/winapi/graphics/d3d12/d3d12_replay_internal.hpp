// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d12_replay_internal.hpp
/// @brief The ExecuteCommandLists replay shared by the d3d12_replay*.cpp
///        files: the replay constants, is_d24_family, ReplayResources,
///        ReplayState and the Replayer class. The constants and
///        is_d24_family sit in an anonymous namespace (is_d24_family is
///        inline so the files that do not use it do not warn). Replayer
///        members whose callers all live in one file keep internal
///        linkage, as they had in d3d12_replay.cpp. Include from those
///        files only. Split out of d3d12_replay.cpp as verbatim code
///        motion.

#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "winapi/graphics/d3d12/d3d12_sampler_record_cache.hpp"
#include "graphics/shaders/ia_cut_rewrite.hpp"

#include <functional>

namespace vitreo::winapi::d3d12 {

namespace {

constexpr u64 CONSTANT_CHUNK_BYTES = 256u * 1024u;
constexpr u32 STAGE_VERTEX = 0;
constexpr u32 STAGE_PIXEL = 1;
constexpr u32 STAGE_COMPUTE = 2;
/// A mesh pipeline's amplification and mesh shaders (graphics root
/// arguments, D3D12_SHADER_VISIBILITY_AMPLIFICATION / _MESH).
constexpr u32 STAGE_AMPLIFICATION = 3;
constexpr u32 STAGE_MESH = 4;

/// Register class of descriptor set N.
constexpr u32 SET_CBV = 0;
constexpr u32 SET_SRV = 1;
constexpr u32 SET_SAMPLER = 2;
constexpr u32 SET_UAV = 3;

/// The cut value of a pipeline's IBStripCutValue (DISABLED: none).
[[nodiscard]] inline graphics::IndexCut strip_cut_of(const GraphicsState& gs) {
    switch (gs.strip_cut) {
    case D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF: return {true, 0xFFFFu};
    case D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF: return {true, 0xFFFFFFFFu};
    default: return {};
    }
}

[[nodiscard]] inline bool is_d24_family(u32 format) {
    return format >= DXGI_FORMAT_R24G8_TYPELESS &&
           format <= DXGI_FORMAT_X24_TYPELESS_G8_UINT;
}

}  // namespace

/// The values ResolveQueryData writes for one statistics query, read once
/// the GPU work it counted has completed: the 14 counters of
/// D3D12_QUERY_DATA_PIPELINE_STATISTICS1 (the first 11 are
/// ..._PIPELINE_STATISTICS), or {NumPrimitivesWritten,
/// PrimitivesStorageNeeded} for SO_STATISTICS_STREAMn
/// (d3d12_replay_statistics.cpp).
void statistics_values(const StatisticsResult& result, std::array<u64, 14>& out);

/// Backend objects the replay keeps between lists.
struct ReplayResources {
    /// Descriptor tables per stage (VS, PS, CS) and set. The backend
    /// snapshots a table when a draw is recorded, so one table per stage and
    /// set is rewritten for every draw.
    std::array<std::array<std::unique_ptr<graphics::BackendDescriptorTable>, 4>,
               5>
        tables;
    /// What each stage's sampler table was last filled from. A draw whose
    /// sampler sources (the same records, statics and table), sampler heap
    /// generation and shader all match leaves the table as it is and reuses
    /// the sampler-state records: an unbounded or SM 6.6 sampler table is
    /// the whole 2048-sampler heap, and refilling it cost ~140 us a draw
    /// (measured, M4 Pro: 2000 dispatches of a 2048-sampler table took
    /// ~150 us each, a window-only control ~8 us).
    struct SamplerMemo {
        bool valid{false};
        const void* shader{nullptr};
        const graphics::BackendDescriptorTable* table{nullptr};
        const void* heap{nullptr};
        u64 generation{0};
        std::vector<std::pair<u8, const void*>> sources;
        std::vector<u32> records;
        std::array<u32, 16> bias{};
        u32 bias_mask{0};
        u32 point_mip_mask{0};
        u32 nudge_mask{0};
        /// The records in a buffer of their own, bound again by a draw that
        /// matches instead of uploading them (2048 x 48 bytes a draw).
        std::shared_ptr<const graphics::BackendBuffer> record_buffer;
        /// bind_stage's sampler_fast_key of the fill (0: none).
        u64 fast_key{0};
    };
    std::array<SamplerMemo, 5> sampler_memo;
    SamplerRecordBufferCache sampler_record_buffers;
    /// Record buffers a memo replaced, kept until the recording generation
    /// that last read them is complete.
    std::vector<std::pair<std::shared_ptr<const graphics::BackendBuffer>, u64>> retired_sampler_records;
    /// Root-constant upload ring: CPU-visible chunks, reused once the GPU
    /// has finished the recording generation that last read them.
    struct Chunk {
        std::unique_ptr<graphics::BackendHeap> heap;
        u64 used{0};
        u64 retire_generation{0};
    };
    Chunk current;
    std::vector<Chunk> retired;
    /// Read by a vertex stream the input layout reads with nothing bound
    /// (Learn: a fetch past an unbound or short buffer returns 0).
    std::unique_ptr<graphics::BackendHeap> zeros;
    /// Scratch textures recorded work still reads, and the recording
    /// generation after which they are free.
    struct Scratch {
        std::unique_ptr<graphics::BackendTexture2D> texture;
        u64 generation{0};
    };
    std::vector<Scratch> scratch_textures;
    /// Upload heaps of large ResolveQueryData calls, kept until the GPU has
    /// run the copy out of them.
    struct ScratchHeap {
        std::shared_ptr<graphics::BackendHeap> heap;
        u64 generation{0};
    };
    std::vector<ScratchHeap> scratch_heaps;
    /// A SUSPENDING_PASS whose RESUMING_PASS has not run yet in this
    /// ExecuteCommandLists. The copies are the local render-target state.
    struct SuspendedPass {
        bool active{false};
        bool allow_uav{false};
        std::vector<cmd::RenderPassSurface> surfaces;
        struct Local {
            std::unique_ptr<graphics::BackendTexture2D> copy;
            cmd::RenderPassSurface surface{};
            /// Texture3D surface: the bound mip's W window, in its own
            /// volume. `copy` stays empty.
            std::unique_ptr<graphics::BackendTexture3D> volume;
        };
        std::vector<Local> locals;
    };
    SuspendedPass suspended;
    /// Backend-only GPU memory recorded work still uses (a deserialized
    /// acceleration structure's build scratch), free after `generation`.
    struct Transient {
        std::unique_ptr<graphics::BackendHeap> heap;
        u64 generation{0};
    };
    std::vector<Transient> transient_heaps;
    /// The heap window a draw binds when no shader-visible heap supplies
    /// its range: the sentinel null slot alone.
    HeapWindowMirror null_window;
};

/// The state a list replays with.
struct ReplayState {
    PipelineState* pso{nullptr};
    /// Push member 14 of the vertex stage (graphics::kLayerStateOffset):
    /// kLayerLimitKnown | the bound views' slice count, and the view
    /// instance's RenderTargetArrayIndex added to a written index.
    u32 layer_limit{0};
    u32 layer_addend{0};
    /// SetPipelineState1's raytracing pipeline.
    GuestAddr state_object{0};
    u64 state_object_serial{0};
    /// ViewInstancingTier 3: the draw being replayed runs its views itself
    /// from PipelineState::vi_first on (Replayer::for_each_view is then
    /// called at that stage, not around the whole draw).
    bool vi_deferred{false};
    /// for_each_view's shader_layer for the deferred draw.
    bool vi_shader_layer{false};
    /// The view of a deferred draw being replayed (0 outside one).
    u32 vi_view{0};
    /// PIPELINE_STATISTICS counters the current work must not add to (the
    /// input assembler of views after the first: "For the stages that come
    /// before X, view instances do not increase those stages' pipeline
    /// statistics").
    u32 statistics_skip{0};
    /// 0 graphics, 1 compute.
    std::array<std::shared_ptr<const RootLayout>, 2> roots;
    std::array<DescriptorHeap*, 2> heaps{};
    struct RootArg {
        bool set{false};
        u64 table{0};
        u64 va{0};
        std::vector<u32> constants;
    };
    std::array<std::vector<RootArg>, 2> args;
    u32 topology{D3D_PRIMITIVE_TOPOLOGY_UNDEFINED};
    std::array<VertexBufferView, 32> vbs{};
    cmd::SetIndexBuffer ib{0, 0, 0};
    std::vector<Viewport> viewports;
    std::vector<Rect> scissors;
    cmd::SetRenderTargets targets{};
    std::array<float, 4> blend_factor{1.0f, 1.0f, 1.0f, 1.0f};
    u32 stencil_ref{0};
    /// The occlusion query begun and not yet ended: every draw's samples
    /// count into it.
    std::shared_ptr<graphics::GpuOcclusionCounter> occlusion;
    /// PIPELINE_STATISTICS[1] and SO_STATISTICS_STREAMn queries begun and
    /// not yet ended in this list, and the GPU records of the interval the
    /// draws now add to (a Begin or End starts a new one, so a record is
    /// shared exactly by the queries open over all of it).
    struct OpenStatistics {
        GuestAddr heap{0};
        u32 index{0};
        std::shared_ptr<StatisticsResult> result;
    };
    std::vector<OpenStatistics> statistics;
    std::shared_ptr<graphics::BackendBuffer> statistics_interval;
    std::shared_ptr<graphics::BackendBuffer> so_interval;
    /// SetPredication's verdict: the predicated operations are skipped.
    bool predicated_off{false};
    /// SetViewInstanceMask (default 0: every view disabled, for pipelines
    /// with D3D12_VIEW_INSTANCING_FLAG_ENABLE_VIEW_INSTANCE_MASKING).
    u32 view_mask{0};
    /// Beginning clears of a matched RESUMING_PASS still in front of the
    /// replay cursor. The suspended pass already applied them.
    u32 skip_pass_clears{0};
};

class Replayer {
public:
    Replayer(Frontend& frontend, ReplayResources& resources)
        : f(frontend), rr(resources) {}

    __attribute__((internal_linkage))
    void run(const std::vector<Command>& commands, ReplayState& state);

private:
    Frontend& f;
    ReplayResources& rr;

    struct BindingSource {
        enum class Kind : u8 { None, Record, View, Constants, Static, Direct, Translation } kind{};
        const Descriptor* record{nullptr};
        u64 va{0};
        u32 parameter{0};
        const SamplerState* sampler{nullptr};
    };
    // Replay-local capacity, shared sequentially by stage binds. Contents are
    // cleared on every call; no descriptor value is cached across draws.
    std::array<std::vector<BindingSource>, 4> m_binding_sources;

    __attribute__((internal_linkage))
    graphics::BackendDescriptorTable* table(u32 stage, u32 set, u32 slots,
                                            graphics::BackendDescriptorClass klass);
    bool upload(const void* data, usize bytes, graphics::BackendBuffer*& buffer,
                u64& offset);
    Resource* resource(GuestAddr address, u64 serial) {
        return f.find_serial<Resource>(address, serial, Kind::Resource);
    }
    /// Bind @p d; returns the view's size in 32-bit words for a raw or
    /// structured buffer view (the push block's size table), else 0.
    /// @p bound is true only when a real resource was written to the slot.
    __attribute__((internal_linkage))
    /// @p uint_view: the slot is a float typed UAV the shader declared as a
    /// uint image for its atomics (CompiledShader::uint_view_uavs); an
    /// R32_FLOAT view is bound as R32_UINT.
    u32 bind_view(graphics::BackendDescriptorTable& t, unsigned slot,
                  const Descriptor& d, bool uav, bool& bound,
                  bool uint_view = false);
    /// @p heap's window mirror (rr.null_window for a null heap), made on
    /// first use, with every slot written since the last call resolved.
    /// Null when the backend cannot make it.
    /// @p aux: the shader reads the auxiliary region (HeapWindowMirror::aux).
    __attribute__((internal_linkage))
    HeapWindowMirror* sync_heap_window(DescriptorHeap* heap, bool aux);
    /// Append @p d's buffer residency record (graphics::kTiledResidency*)
    /// to @p words and return its word offset; 0 for a view of a resource
    /// that is not a tiled buffer.
    __attribute__((internal_linkage))
    u32 append_buffer_residency(const Descriptor& d, std::vector<u32>& words);
    /// The window's buffer residency records, by id, for @p heap (null: no
    /// records), into @p words.
    __attribute__((internal_linkage))
    void window_residency_words(DescriptorHeap* heap, HeapWindowMirror& window,
                                std::vector<u32>& words);
    /// Write slot @p slot of @p window from @p d.
    __attribute__((internal_linkage))
    void resolve_heap_window_slot(HeapWindowMirror& window, u32 slot,
                                  const Descriptor* d);
    /// Words 1-7 of the last typed view bind_view bound as raw memory
    /// (graphics::kTypedBufferRaw in its returned entry).
    std::array<u32, 7> m_typed_raw_words{};

    bool bind_stage(ReplayState& st, u32 stage, const CompiledShader& shader,
                    std::array<const graphics::BackendDescriptorTable*,
                               graphics::BACKEND_MAX_DESCRIPTOR_SETS>& out,
                    std::vector<u32>& push);
    /// What each real slot of a tiled-status shader is bound to (null / 0
    /// when nothing): a table descriptor, or a root descriptor's GPU VA.
    struct TiledStatusSources {
        std::vector<const Descriptor*> srv;
        std::vector<u64> srv_va;
        std::vector<const Descriptor*> uav;
        std::vector<u64> uav_va;
        std::vector<const SamplerState*> samplers;
    };
    /// Tiled resources tier 2 (CompiledShader::tiled_status): residency
    /// companions, status twins and buffer residency records, after the
    /// stage's tables are bound.
    bool bind_tiled_status(u32 stage, const CompiledShader& shader,
                           const TiledStatusSources& in);
    /// @p indirect: the counts come from that GPU buffer (ExecuteIndirect).
    void draw(ReplayState& st, bool indexed, const cmd::DrawIndexed& d,
              const graphics::BackendIndirectArgs* indirect = nullptr);
    /// Runs @p one once per enabled view instance of st.pso (once when view
    /// instancing is off) with that view's pipeline, viewports, scissors
    /// and render targets in @p st; restores them after. @p shader_layer:
    /// the translated stage applies the render-target array index location
    /// itself (push member 14) instead of the views being re-sliced.
    void for_each_view(ReplayState& st, bool shader_layer,
                       const std::function<void()>& one);
    /// One view instance of draw (the whole draw when view instancing is
    /// off).
    void draw_view(ReplayState& st, bool indexed, const cmd::DrawIndexed& d,
                   const graphics::BackendIndirectArgs* indirect);
    /// IBStripCutValue on an indexed draw that Metal draws itself: when the
    /// index data may hold a value Metal's restart treats differently
    /// (ia_cut_rewrite.hpp), @p call draws a rewritten 32-bit stream that
    /// @p kept owns. False: nothing to draw, or the rewrite failed loudly.
    bool apply_strip_cut(
        ReplayState& st, const cmd::DrawIndexed& counts,
        const graphics::BackendIndirectArgs* indirect,
        graphics::BackendDrawCall& call,
        std::vector<std::unique_ptr<graphics::BackendBuffer>>& kept);
    /// Raster state, render pass and draw of a call whose pipeline and
    /// vertex bindings are already filled. True when the draw was recorded.
    bool submit_draw(ReplayState& st, graphics::BackendDrawCall& call);
    /// A pipeline whose geometry, hull or domain stage was lowered.
    /// A geometry or tessellation draw. @p indirect set: its counts are in
    /// a GPU buffer and the CPU never reads them (assemble_on_gpu).
    void draw_lowered(ReplayState& st, bool indexed, const cmd::DrawIndexed& d,
                      const graphics::BackendIndirectArgs* indirect = nullptr);
    /// Primitive assembly and the vertex capture of a GPU-sourced lowered
    /// draw, on the GPU timeline: vitreo_ia_resolve (ia_resolve.hpp) reads
    /// the argument block and writes the primitive keys and counts, and one
    /// relative capture draw, indirect from the resolved block, records
    /// every instance. @p primitives is the capacity the later passes are
    /// sized for; @p resolved holds the GPU counts.
    bool assemble_on_gpu(
        ReplayState& st, bool indexed,
        const graphics::BackendIndirectArgs& indirect, u32 group,
        graphics::PrimitiveAssembly assembly, bool tess, u32 record_vec4s,
        const std::array<u32, graphics::BACKEND_MAX_VERTEX_BUFFERS>& strides,
        std::vector<std::unique_ptr<graphics::BackendBuffer>>& kept,
        u32& primitives, graphics::BackendBuffer*& records,
        graphics::BackendBuffer*& ids,
        std::shared_ptr<graphics::BackendBuffer>& resolved);
    /// The input-assembler streams of @p call from st.vbs: one backend
    /// stream per slot the layout reads (an unbound one reads zeros), or,
    /// for a vertex-pulling layout (GraphicsState::vertex_pulling), the
    /// pull table of every slot's {address, stride, SizeInBytes}.
    /// A capture drawing instance @p instance of a draw from
    /// StartInstanceLocation @p start alone (first instance start +
    /// instance) passes both, so pulled per-instance data steps as in the
    /// real draw (VERTEX_PULL_TABLE_HEADER).
    bool bind_vertex_streams(ReplayState& st, graphics::BackendDrawCall& call,
                             bool per_instance_capture = false, u32 start = 0,
                             u32 instance = 0);
    /// Constant buffers a flat-bound lowered stage reads, from the graphics
    /// root signature. Unbound slots read as empty (limit 0).
    bool bind_flat_constants(
        ReplayState& st, u32 visibility, const CompiledShader& shader,
        std::array<const graphics::BackendBuffer*,
                   graphics::BACKEND_MAX_CONSTANT_BUFFER_SLOTS>& buffers,
        std::array<u64, graphics::BACKEND_MAX_CONSTANT_BUFFER_SLOTS>&
            offsets,
        std::vector<u32>& push);
    void dispatch(ReplayState& st, const cmd::Dispatch& d,
                  const graphics::BackendIndirectArgs* indirect = nullptr);
    /// DispatchMesh, or (with @p indirect) one DISPATCH_MESH command of
    /// ExecuteIndirect whose arguments the GPU reads.
    void dispatch_mesh(ReplayState& st, const cmd::DispatchMesh& d,
                       const graphics::BackendIndirectArgs* indirect = nullptr);
    /// One view instance of dispatch_mesh.
    void dispatch_mesh_view(ReplayState& st, const cmd::DispatchMesh& d,
                            const graphics::BackendIndirectArgs* indirect);
    /// A raster-capture mesh draw (d3d12_replay_mesh.cpp): @p call (and its
    /// device-staging @p chunks) appends the surviving primitives to a
    /// capture in a pass of its own, then pass C draws them.
    bool submit_mesh_raster(
        ReplayState& st, graphics::BackendDrawCall& call,
        const std::vector<std::pair<std::array<u32, 3>, std::array<u32, 3>>>& chunks);
    /// An indirect DispatchMesh resolved on the GPU into the chunks of a
    /// device-staging mesh pipeline without an amplification shader.
    bool submit_mesh_indirect_chunks(ReplayState& st, graphics::BackendDrawCall& call,
                                     const graphics::BackendIndirectArgs& indirect);
    /// Target-independent / conservative rasterization pass C
    /// (raster_emulation.hpp) over @p primitives triangles of @p records,
    /// through record ids at @p ids, up to the GPU's count at word
    /// @p limit_word of @p limit (at @p limit_offset) when bound.
    bool raster_pass_c(ReplayState& st, const std::string& msl,
                       std::unique_ptr<graphics::BackendPipeline>& slot,
                       graphics::BackendBuffer* records, u64 records_offset,
                       graphics::BackendBuffer* ids, u64 primitives,
                       graphics::BackendBuffer* limit, u64 limit_offset, u32 limit_word);
    /// @p bytes of an indirect argument block as the GPU leaves them after
    /// all work recorded so far (an ordered copy and a wait).
    bool read_indirect_words(const graphics::BackendIndirectArgs& indirect, u32* words,
                             usize bytes);
    /// ViewInstancingTier 3 of a mesh pipeline whose amplification or mesh
    /// shader runs once (d3d12_replay_view_mesh.cpp).
    void dispatch_mesh_views(ReplayState& st, const cmd::DispatchMesh& d,
                             const graphics::BackendIndirectArgs* indirect);
    /// CPU-visible upload space: @p bytes at @p host, which the GPU reads
    /// at (@p buffer, @p offset).
    bool reserve(usize bytes, graphics::BackendBuffer*& buffer, u64& offset,
                 u8*& host);
    /// @p bytes of @p r at @p offset as the GPU leaves them after all work
    /// recorded so far (submits and waits).
    bool gpu_read(Resource& r, u64 offset, void* out, usize bytes);
    void query(ReplayState& st, const cmd::Query& c);
    void resolve_query(const cmd::ResolveQuery& c);

    // -- PIPELINE_STATISTICS / SO_STATISTICS (d3d12_replay_statistics.cpp) --
    /// GPU work a statistics query counts from one draw, recorded before the
    /// draw (input assembly) and after it (the clipper over the vertex
    /// shader's captured outputs, vitreo_stats_primitives).
    struct StatisticsDraw {
        std::shared_ptr<graphics::BackendBuffer> record;
        std::shared_ptr<graphics::BackendBuffer> capture;
        std::shared_ptr<graphics::BackendBuffer> ids;
        std::shared_ptr<graphics::BackendBuffer> resolved;
        graphics::StatsKernelParams clip{};
        bool active{false};
    };
    [[nodiscard]] bool pipeline_statistics_open(const ReplayState& st) const;
    /// The open interval's pipeline record (opened on first use and handed
    /// to every open statistics query); null when none is open or it could
    /// not be allocated (then every counter is marked unmeasured).
    std::shared_ptr<graphics::BackendBuffer> statistics_record(ReplayState& st);
    void add_statistic(ReplayState& st, u32 counter, u64 value);
    /// A mesh draw in an open PIPELINE_STATISTICS interval: the record its
    /// counting pipeline adds to (null when it cannot be counted, which is
    /// then reported), and the pipeline in @p pipeline.
    std::shared_ptr<graphics::BackendBuffer> mesh_statistics(
        ReplayState& st, const graphics::BackendPipeline*& pipeline);
    void statistics_unmeasured(ReplayState& st, u32 bits, const char* why);
    /// Where a stream-output pass adds its {needed, written} counts: the
    /// open SO_STATISTICS interval, or scratch nobody reads.
    graphics::BackendBuffer* so_statistics_record(ReplayState& st);
    /// A draw that reaches the rasterizer without a lowered stage:
    /// IAVertices, IAPrimitives, VSInvocations, CInvocations (CPU when the
    /// draw is direct and non-indexed, else vitreo_ia_resolve +
    /// vitreo_stats_vertices), and the clipper plan @p plan for CPrimitives.
    void count_draw(ReplayState& st, bool indexed, const cmd::DrawIndexed& d,
                    const graphics::BackendIndirectArgs* indirect,
                    bool rasterized, StatisticsDraw& plan);
    void finish_statistics_draw(StatisticsDraw& plan);
    /// A lowered draw's input assembly, from the CPU-assembled keys of one
    /// instance (@p ids, @p group keys a primitive).
    void count_lowered_input(ReplayState& st, const std::vector<u32>& ids,
                             u32 group, u32 vertex_count, u32 instances,
                             bool tessellated);
    /// One geometry expansion (vitreo_stats_geometry).
    void count_geometry(ReplayState& st, u32 invocations,
                        graphics::BackendBuffer* counter,
                        graphics::BackendBuffer* expanded, bool rasterized);
    /// One software-tessellator batch (vitreo_stats_tessellation).
    /// CInvocations / CPrimitives of a raster-emulated draw of the vertex
    /// shader's own triangles: @p primitives triangles whose record ids are
    /// at @p ids, clipped over the captured @p records from byte
    /// @p records_offset (PipelineState::RasterProgram::clip).
    void count_raster_clipper(ReplayState& st, u32 primitives,
                              graphics::BackendBuffer* ids,
                              graphics::BackendBuffer* records,
                              u32 records_offset, u32 record_vec4s);
    /// CInvocations / CPrimitives of @p primitives primitives of the
    /// captured vertex-shader records at @p records + @p records_offset,
    /// through their record ids (PipelineState::per_vertex_primitive each).
    void count_clip(ReplayState& st, graphics::BackendBuffer* records,
                    u64 records_offset, graphics::BackendBuffer* ids,
                    u64 primitives);
    void count_tessellation(ReplayState& st, u32 vpp, u32 index_capacity,
                            graphics::BackendBuffer* block,
                            graphics::BackendBuffer* indices,
                            graphics::BackendBuffer* records, u32 record_vec4s,
                            bool rasterized);
    void count_dispatch(ReplayState& st, const cmd::Dispatch& d,
                        const graphics::BackendIndirectArgs* indirect);
    /// D3D11.3 15.2 (the pipeline D3D12 runs): rasterization is disabled
    /// when the pixel shader is NULL and depth and stencil are both off.
    [[nodiscard]] bool rasterization_disabled(const ReplayState& st) const;
    void set_predication(ReplayState& st, const cmd::SetPredication& c);
    __attribute__((internal_linkage))
    void execute_indirect(ReplayState& st, const cmd::ExecuteIndirect& c);
    __attribute__((internal_linkage))
    void write_immediate(const cmd::WriteImmediate& c);
    void copy_texture_region(const cmd::CopyTextureRegion& c);
    void resolve(Resource& dst, Resource& src, const cmd::ResolveSubresource& c);
    /// ResolveSubresourceRegion ENCODE_ / DECODE_SAMPLER_FEEDBACK
    /// (d3d12_replay_feedback.cpp).
    void transcode_feedback(Resource& dst, Resource& src, const cmd::ResolveSubresource& c);
    __attribute__((internal_linkage))
    void copy_volume_footprint(const cmd::CopyTextureRegion& c, Resource& buffer,
                               Resource& texture, bool to_buffer);
    __attribute__((internal_linkage))
    void copy_depth_stencil_plane(const cmd::CopyTextureRegion& c,
                                  Resource& buffer, Resource& texture, u32 mip,
                                  u32 slice, u32 plane, bool to_buffer);
    void copy_resource(const cmd::CopyResource& c);
    void clear_render_target(const cmd::ClearRenderTarget& c);
    void clear_depth_stencil(const cmd::ClearDepthStencil& c);
    void clear_unordered_access(const cmd::ClearUnorderedAccess& c);
    __attribute__((internal_linkage))
    void set_root_signature(ReplayState& st, const cmd::SetRootSignature& c);
    // -- raytracing (d3d12_replay_raytracing.cpp) --
    void dispatch_rays(ReplayState& st, const cmd::DispatchRays& c);
    /// During dispatch_rays: what the megakernel's reserved bindings
    /// (CompiledShader::rt_reserved) are bound to.
    struct RtDispatchBindings {
        std::array<u64, 4> tables{};
        graphics::BackendBuffer* constants{nullptr};
        u64 constants_offset{0};
        /// Local root descriptors (dxil_raytracing.hpp): the VA of each
        /// element of each view array (element 0 null), and the
        /// translation buffer.
        std::vector<std::vector<u64>> local_views;
        graphics::BackendBuffer* translation{nullptr};
        u64 translation_offset{0};
        u32 translation_words{0};
    };
    /// The records' local root descriptors into @p b and the record bases
    /// into @p words; false (nothing dispatched) when they cannot be read.
    bool resolve_local_views(const StateObject& so, const cmd::DispatchRays& c, u32* words,
                             RtDispatchBindings& b);
    const RtDispatchBindings* rt_dispatch_{nullptr};
    /// A direct draw sent through assemble_on_gpu (draw_view): the most
    /// primitives and vertex records it can produce, so the passes are sized
    /// for it rather than for the indirect capacity. 0: unknown (indirect).
    u32 lowered_primitive_bound_{0};
    u32 lowered_record_bound_{0};
    /// Bind the top-level structure at @p va (an empty one when @p va names
    /// none: a NULL acceleration structure misses every ray).
    bool bind_acceleration_structure(graphics::BackendDescriptorTable& t, unsigned slot, u64 va);
    /// @p bytes from the CPU into guest GPU memory at @p va, in stream order.
    bool write_va(const u64 va, const void* data, usize bytes);
    bool copy_va(u64 dst, u64 src, u64 bytes);
    /// @p bytes at @p va as the GPU leaves them after everything recorded.
    bool read_va(u64 va, void* out, usize bytes);
    /// Device-local scratch kept until the GPU is past this recording.
    graphics::BackendBuffer* transient_buffer(u64 bytes);
    /// The record at @p va with a backend structure of at least
    /// @p backend_bytes (not @p keep_off, an update's distinct source).
    AccelerationStructure* place_structure(u64 va, bool top_level, u64 backend_bytes,
                                           const graphics::BackendAccelerationStructure* keep_off);
    bool build_bottom_geometry(const RtInputs::Geometry& g, graphics::BackendBuffer* blob, u64 at,
                               graphics::BackendRtGeometry& out);
    void build_acceleration_structure(const cmd::BuildAccelerationStructure& c);
    void copy_acceleration_structure(const cmd::CopyAccelerationStructure& c);
    void deserialize_acceleration_structure(const cmd::CopyAccelerationStructure& c);
    void emit_postbuild(u64 dest, u32 type, const std::vector<u64>& sources);
};

}  // namespace vitreo::winapi::d3d12
