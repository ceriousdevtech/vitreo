// SPDX-License-Identifier: LGPL-2.1-only
// @file d3d11_manager_statistics_stream_out.inc
// @brief D3D11Manager class body, part 15 of 17: PIPELINE_STATISTICS, stream
//   output, the reference-count model, the context memo and compute dispatch.
//
// A fragment of the D3D11Manager definition in d3d11.hpp, which
// includes it inside the class, in this order; not a standalone header.

    /// Add a guest draw to the PIPELINE_STATISTICS queries building on
    /// @p context (called once per API draw, before any emulation pass).
    /// @p indirect_arguments / @p indirect_offset name the argument block of
    /// an indirect draw (the counts are then unused).
    /// PIPELINE_STATISTICS work a rasterized input-assembler draw (no
    /// geometry or tessellation stage) leaves to execute_draw: its vertex
    /// shader captures its outputs into `capture` while it draws (keyed
    /// draw-relative), and vitreo_stats_primitives then clips the captured
    /// positions into `record` (CPrimitives).
    struct StatisticsDraw {
        std::shared_ptr<graphics::BackendBuffer> record;
        std::shared_ptr<graphics::BackendBuffer> capture;
        std::shared_ptr<graphics::BackendBuffer> ids;
        std::shared_ptr<graphics::BackendBuffer> resolved;
        graphics::StatsKernelParams clip{};
        bool active{false};
    };
    void count_pipeline_draw(
        GuestAddr context, bool indexed, u32 count, u32 start_location,
        i32 base_vertex, u32 instance_count,
        const graphics::BackendBuffer* indirect_arguments,
        u32 indirect_offset, StatisticsDraw& plan);
    /// Whether a PIPELINE_STATISTICS query is building on @p context.
    [[nodiscard]] bool pipeline_statistics_building(GuestAddr context) const;
    /// The open statistics interval's GPU record on @p context (opened and
    /// handed to every statistics query building there), or null when none
    /// is building; a record that could not be allocated marks the GPU
    /// counters unmeasured.
    [[nodiscard]] std::shared_ptr<graphics::BackendBuffer>
    statistics_record_for_draw(GuestAddr context);
    /// The work just recorded on @p context wrote the open statistics
    /// interval's record: every statistics query building there is final
    /// only once the generation that work went into has completed
    /// (recorded_work_generation).
    void note_statistics_recorded(GuestAddr context);
    /// DIAGNOSTIC, observation only, with the skip-stats switch on
    /// (d3d11_skip_stats_enabled; the caller checks it): every 5 s, the draws
    /// the backend's encoder reopened a pass for, and dropped, since the last
    /// report (GraphicsBackendDevice::draw_encode_counts).
    void note_draw_encode_counts();
    /// The submission generation the work this thread just recorded went
    /// into (GraphicsBackendDevice::last_recorded_generation), read AFTER
    /// recording it: a recording_generation() read before can name a
    /// recording another thread submits before the work joins it, and a
    /// count read one generation early. recording_generation() when this
    /// thread has recorded nothing on the immediate stream (0 while it
    /// captures a command list: untracked).
    [[nodiscard]] u64 recorded_work_generation();
    /// Mark @p bits (pipeline_statistic_bit) unmeasured in every statistics
    /// query building on @p context, naming @p why once.
    void mark_statistics_unmeasured(GuestAddr context, u32 bits,
                                    const char* why);
    /// vitreo_stats_* (graphics/shaders/pipeline_stats.hpp), built on first
    /// use; null when the backend cannot build it.
    [[nodiscard]] graphics::BackendComputePipeline* statistics_kernel(
        const char* entry_point);
    /// Dispatch vitreo_stats_primitives for a draw count_pipeline_draw
    /// planned, once the draw is recorded.
    void finish_statistics_draw(StatisticsDraw& plan);
    /// A geometry-shader expansion's GSInvocations, GSPrimitives and the
    /// rasterized stream's CInvocations / CPrimitives
    /// (vitreo_stats_geometry).
    void count_geometry_statistics(GuestAddr context, GeometryShader& gs,
                                   u32 invocations,
                                   graphics::BackendBuffer* counter,
                                   graphics::BackendBuffer* expanded,
                                   bool rasterized);
    /// A software-tessellator batch's DSInvocations and, rasterized, its
    /// CInvocations / CPrimitives (vitreo_stats_tessellation).
    void count_tessellation_statistics(GuestAddr context,
                                       const DomainShader& ds, u32 vpp,
                                       u32 index_capacity,
                                       graphics::BackendBuffer* block,
                                       graphics::BackendBuffer* indices,
                                       graphics::BackendBuffer* records,
                                       bool rasterized);
    /// D3D11.3 15.2: rasterization is disabled when the pixel shader is
    /// NULL and DepthEnable and StencilEnable are both FALSE.
    [[nodiscard]] bool rasterization_disabled(const ContextState& state) const;
    /// The bound rasterizer state's DepthClipEnable (TRUE by default).
    [[nodiscard]] bool depth_clip_enabled(const ContextState& state) const;
    /// Add a dispatch: @p groups is gx*gy*gz for a direct dispatch;
    /// @p indirect_arguments / @p indirect_offset name the argument block of
    /// an indirect one.
    void count_pipeline_dispatch(
        GuestAddr context, u64 groups, u64 threads_per_group,
        const graphics::BackendBuffer* indirect_arguments,
        u32 indirect_offset);
    /// Whether @p query's data is final: its End point and the GPU work its
    /// bracket counted have completed.
    [[nodiscard]] bool query_result_ready(const Query& query);
    [[nodiscard]] bool query_needs_flush(const Query& query);
    /// std::hash of an MSL source a draw keys its pipeline on. With
    /// @p shader_owned the value is kept by string object: a shader's MSL
    /// strings and variants never change once built and live until the
    /// shader is destroyed, which clears the memo (forget_msl_hashes).
    /// Rehashing tens of KB of source twice per draw cost the RHI thread
    /// more than the pipeline lookup it keyed.
    [[nodiscard]] u64 msl_hash(const std::string& msl, bool shader_owned);
    void forget_msl_hashes() noexcept { m_msl_hashes.clear(); }
    static constexpr u64 kGpuUseUntracked = ~u64{0};
    /// Record that the GPU work this thread just recorded reads or writes
    /// @p buffer. Its generation is the one the work joined
    /// (recorded_work_generation), not recording_generation() read after
    /// recording: another thread's submit in between made that the next,
    /// empty recording, and Map(DO_NOT_WAIT) answered WAS_STILL_DRAWING until
    /// unrelated work happened to be submitted.
    void note_gpu_use(Buffer& buffer) {
        const u64 generation = recorded_work_generation();
        if (generation == 0) {
            buffer.gpu_use_gen = kGpuUseUntracked;
        } else if (buffer.gpu_use_gen != kGpuUseUntracked &&
                   generation > buffer.gpu_use_gen) {
            buffer.gpu_use_gen = generation;
        }
    }
    /// After a draw (@p compute false) or dispatch on @p context: every
    /// mappable DEFAULT buffer (MapOnDefaultBuffers) bound to it as a UAV was
    /// written on the GPU, so its next Map waits for that work and reads it
    /// back (map_buffer).
    void note_uav_buffer_writes(GuestAddr context, bool compute);
    [[nodiscard]] bool execute_stream_out(GuestAddr context, bool indexed,
                                          u32 count, u32 start_location,
                                          i32 base_vertex, u32 instance_count,
                                          u32 first_instance,
                                          const IndirectDraw& indirect);
    /// Stream output of a vertex shader for an indirect draw: the counts
    /// are resolved on the GPU timeline. Returns whether it was recorded.
    [[nodiscard]] bool execute_stream_out_indirect(
        GuestAddr context, bool indexed, const IndirectDraw& indirect);
    /// The GPU-side filled size of an SO-capable buffer (Buffer::so_filled),
    /// created zeroed on first use. Null when it cannot be allocated.
    [[nodiscard]] graphics::BackendBuffer* stream_out_filled_word(
        Buffer& buffer);
    /// The accounting / repack / DrawAuto pipelines, built on first use.
    [[nodiscard]] bool ensure_stream_out_pipelines();
    /// A zeroed stats record for one streaming draw, or the shared scratch
    /// record when no stream-output query is building on @p context.
    [[nodiscard]] std::shared_ptr<graphics::BackendBuffer>
    stream_out_stats_record(GuestAddr context);
    /// One stream's share of a streaming draw: its declaration entries
    /// (resolved to record slots), where the vertices come from, and how many
    /// primitives there are -- CPU-known, or a GPU word.
    struct StreamOutPass {
        u32 stream{0};
        std::vector<graphics::StreamOutRepackEntry> entries;
        std::array<u32, 4> strides{};
        /// Slots this stream's declaration writes.
        u32 slot_mask{0};
        graphics::BackendBuffer* records{nullptr};
        u32 records_offset{0};
        u32 record_vec4s{0};
        graphics::StreamOutIndexMode index_mode{
            graphics::StreamOutIndexMode::None};
        u32 first{0};
        i32 base_vertex{0};
        bool strip{false};
        graphics::BackendBuffer* indices{nullptr};
        u32 indices_offset{0};
        u32 vertices_per_primitive{1};
        /// count_mode 0: `primitives`. 1 / 2: the word at `count` +
        /// `count_offset` holds primitives / vertices (SoPrepareParams).
        u32 primitives{0};
        u32 count_mode{0};
        const graphics::BackendBuffer* count{nullptr};
        u32 count_offset{0};
        /// Threads the repack is dispatched with: the most vertices the pass
        /// can write (it stops at what the accounting allows).
        u32 max_vertices{0};
    };
    /// Account and write one stream (vitreo_so_prepare + vitreo_so_repack).
    /// False when the GPU work could not be recorded.
    [[nodiscard]] bool stream_out_pass(
        GuestAddr context, const StreamOutPass& pass,
        const std::shared_ptr<graphics::BackendBuffer>& stats);
    /// Stream output of one geometry-program expansion (11.29): what pass B
    /// wrote to each stream, in D3D's order (vitreo_so_compact), through
    /// the same accounting and repack as a vertex shader's.
    struct GeometryStreamOutput {
        std::vector<StreamOutDeclEntry> entries;
        std::array<u32, 4> strides{};
        /// Slots each stream's declaration writes (bit n = slot n).
        std::array<u32, 4> stream_slots{};
        /// o# register of each vec4 in an expanded record.
        std::vector<u32> record_registers;
        /// 1 point, 2 line, 3 triangle: the expansion writes lists.
        u32 vertices_per_primitive{1};
        u32 vertices_per_invocation{0};
        /// Invocations: CPU-known, or the most there can be when
        /// `invocation_count` (a GPU word) holds the real number.
        u64 invocations{0};
        const graphics::BackendBuffer* invocation_count{nullptr};
        u32 invocation_count_offset{0};
        /// Invocation slots per stream region of the expansion (the
        /// counter's per-stream stride).
        u32 invocation_stride{0};
    };
    void stream_out_geometry_expansion(
        GuestAddr context, const GeometryStreamOutput& so,
        graphics::BackendBuffer* expanded, graphics::BackendBuffer* counter,
        const std::shared_ptr<graphics::BackendBuffer>& stats);
    /// ID3D11Device::CreateGeometryShaderWithStreamOutput (device slot 14).
    void create_geometry_shader_with_stream_output(Marshaler& m,
                                                   GuestAddr self);
    /// Context slots 23 GSSetShader, 37 SOSetTargets, 93 SOGetTargets and
    /// 38 DrawAuto. GSGetShader (82) is in the shared getter arm.
    void context_gs_set_shader(Marshaler& m, GuestAddr self);
    void context_so_set_targets(Marshaler& m, GuestAddr self);
    void context_so_get_targets(Marshaler& m, GuestAddr self);
    void context_draw_auto(GuestAddr self);
    /// The reference counts of every device child this manager hands out
    /// (Windows' runtime keeps the same two; its debug layer prints them as
    /// "Refcount" and "IntRef"):
    ///  - `public_refs` is the application's: what AddRef, Release and
    ///    QueryInterface report, and what a Get* adds to;
    ///  - `ref_count` is every reference -- the application's plus the
    ///    runtime's own (a view on its resource, a context binding, an
    ///    active context state, a predicate, a recording) -- and the object
    ///    is destroyed when it reaches 0;
    ///  - while `public_refs` > 0 the object holds ONE reference on its
    ///    parent: the device, or for a swap chain's buffer the swap chain.
    /// Measured (d3d11_a12_refmodel_probe): a bound VB whose last app
    /// reference is released returns 0 and drops the device count, IAGet
    /// takes both back; a texture released under its view drops the device
    /// count, GetResource takes it back; a second, deduplicated
    /// CreateBlendState adds no device reference; GetBuffer counts on the
    /// chain, not the device. So nothing the runtime alone keeps alive can
    /// keep the device alive.
    ///
    /// retain_object is the application's AddRef on any object (the
    /// device-level interfaces too, through add_ref); release_object its
    /// Release. Both return the new public count (0 for an unknown address).
    u32 retain_object(GuestAddr address);
    u32 release_object(GuestAddr address);
    /// The parent reference an object holds while the application does.
    void acquire_parent(GuestAddr device_owner, GuestAddr swap_chain = 0);
    void release_parent(GuestAddr device_owner, GuestAddr swap_chain = 0);
    /// Drop one reference through the object's own release_* path, which
    /// destroys it at 0. Never touches the public count.
    void release_internal(GuestAddr address);
    /// Call @p f with the record of the device child at @p address.
    template <typename F>
    bool visit_child(GuestAddr address, F&& f);
    /// retain_object / release_object for a method that already holds the
    /// record of its `self` (the Buffer and SRV methods look it up first).
    /// An address names at most one live object (allocate_object), so this is
    /// the record visit_child would have found, and @p release_kind is the
    /// release_* release_internal would have picked (WP-R1 C4).
    template <typename T>
    [[nodiscard]] u32 retain_child(T& object);
    template <typename T>
    [[nodiscard]] u32 release_child(GuestAddr address, T& object,
                                    u32 (D3D11Manager::*release_kind)(GuestAddr));

    /// WP-R1 (com_direct): the records the last ID3D11DeviceContext call
    /// resolved -- interface -> device, context state, deferred? -- reused by
    /// the next call on the same interface while m_context_lookup_generation
    /// is unchanged. Read and written under the device lock, like every map
    /// it caches.
    struct ContextMemo {
        GuestAddr self{0};
        u64 generation{0};
        Device* device{nullptr};
        ContextState* state{nullptr};
        bool deferred{false};
    };
    ContextMemo m_context_memo{};
    /// Moves whenever a lookup the memo caches could answer differently, or
    /// a record it points at could go away (invalidate_context_lookups). The
    /// lookups read only m_interfaces, m_devices, m_context_states and
    /// m_deferred_contexts, and an unordered_map node never moves, so the
    /// events are: every erase from those four maps, every insert into
    /// m_deferred_contexts, and the recycling of any object address
    /// (free_object). Inserts elsewhere add new keys only, and assignments
    /// into an existing node keep the node.
    u64 m_context_lookup_generation{1};
    void invalidate_context_lookups() noexcept { ++m_context_lookup_generation; }
    /// QueryInterface of an SRV or its IDXGISurface facet.
    [[nodiscard]] u32 srv_query_interface(Marshaler& m, GuestAddr view);
    /// IDXGISurface methods of a buffer's and of an SRV's surface facet.
    void buffer_surface_method(Marshaler& m, usize slot, GuestAddr buffer);
    void srv_surface_method(Marshaler& m, usize slot, GuestAddr view);
    /// ID3D11View::GetResource: write @p resource, AddRef'd, to arg 1.
    void write_view_resource(Marshaler& m, GuestAddr resource);
    /// Bind one stage's b#/t#/s#/u# registers into a compute dispatch.
    std::string describe_dispatch_inputs(
        std::span<const GuestAddr> constant_buffers,
        std::span<const GuestAddr> shader_resources) const;
    void bind_compute_stage(std::span<const GuestAddr> constant_buffers,
                            std::span<const u32> constant_first,
                            std::span<const GuestAddr> shader_resources,
                            std::span<const GuestAddr> samplers,
                            std::span<const GuestAddr> unordered_access_views,
                            const d3d11::ShaderBindingLayout& layout,
                            graphics::BackendDispatchCall& call);
    /// Fill @p counters with the hidden counter of the view bound at each u#
    /// @p layout's shader moves. False when one of those views has none.
    bool bind_uav_counters(
        std::span<const GuestAddr> unordered_access_views,
        const d3d11::ShaderBindingLayout& layout,
        std::span<const graphics::BackendBuffer*,
                  graphics::BACKEND_MAX_UAV_COUNTER_BINDINGS> counters);
    /// As execute_draw: a non-zero @p indirect_args supplies the threadgroup
    /// counts and the three group_count_* arguments are ignored.
    /// A draw or dispatch reached the backend on @p context. On the immediate
    /// context, once at least a minimum chunk of work has been recorded and
    /// the backend says the GPU is running dry, submit it now rather than at
    /// Present (see GraphicsBackendDevice::wants_early_submit).
    void note_immediate_gpu_work(GuestAddr context);
    void execute_dispatch(GuestAddr context, u32 group_count_x,
                          u32 group_count_y, u32 group_count_z,
                          GuestAddr indirect_args = 0,
                          u32 indirect_offset = 0);
    /// Resolve an indirect-argument ID3D11Buffer to its backend buffer.
    ///
    /// Learn (DrawInstancedIndirect / DrawIndexedInstancedIndirect /
    /// DispatchIndirect) all require the buffer to carry
    /// D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, so a buffer without it is a
    /// caller error and the call is dropped with a diagnostic rather than
    /// executed with whatever bytes happen to be there. So is an offset that
    /// is not DWORD aligned and an argument block of @p block_bytes (16, 20
    /// or 12) that does not fit in the buffer at @p offset (measured on WARP,
    /// tools/oracle_probe/d3d11_indirect_args_bounds_probe.expected). Null
    /// when the call is dropped.
    [[nodiscard]] const graphics::BackendBuffer* resolve_indirect_args(
        GuestAddr buffer_interface, u32 offset, const char* api,
        u32 block_bytes);
