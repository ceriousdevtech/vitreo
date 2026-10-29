// SPDX-License-Identifier: LGPL-2.1-only
// @file d3d11_manager_command_records.inc
// @brief D3D11Manager class body, part 5 of 17: command records (deferred
//   recording, DeferredContext, Query, CommandList, Fence, ClassLinkage,
//   ClassInstance).
//
// A fragment of the D3D11Manager definition in d3d11.hpp, which
// includes it inside the class, in this order; not a standalone header.

    /// One ID3D11DeviceContext call a deferred context recorded, replayed on
    /// the executing context by ExecuteCommandList through the same method
    /// dispatch the guest reaches (impl_ContextMethod).
    ///
    /// Learn (ID3D11DeviceContext::ExecuteCommandList / FinishCommandList):
    /// nothing a deferred context records runs before the list executes, and
    /// its arguments are the values at record time. So every pointer argument
    /// is copied into the recording's guest arena and the call keeps a
    /// reference on every object it names; the replay reads those copies.
    struct RecordedCall {
        /// Context vtable slot, or kUploadMappedBlock (a deferred Map).
        u16 slot{0};
        u8 arg_count{0};
        /// Argument slots after `This`, as Marshaler::arg numbers them (x86:
        /// 32-bit stack slots). Pointer arguments point into the arena.
        std::array<u64, 10> args{};
        /// x64: XMM0-XMM3 low words, where the first four FLOAT arguments go
        /// (ClearDepthStencilView's Depth, SetResourceMinLOD's MinLOD).
        std::array<u32, 4> xmm{};
        /// Objects the call references, each holding one reference.
        std::vector<GuestAddr> held;
    };
    /// A deferred Map(WRITE_DISCARD): on replay, map the resource DISCARD on
    /// the executing context and copy the block into it (args: resource,
    /// subresource, block, bytes).
    static constexpr u16 kUploadMappedBlock = 0xFFFFu;
    /// A deferred context's debug marker (annotation_marker): on replay,
    /// emit it on the executing context (args: DebugMarkerKind, UTF-8 label
    /// in the arena, label bytes).
    static constexpr u16 kDebugMarker = 0xFFFEu;

    /// Guest memory owned by one recording: copies of pointer arguments,
    /// deferred-Map blocks and the replay frame. Freed with the recording.
    struct GuestArena {
        struct Chunk {
            GuestAddr base{0};
            usize size{0};
            usize used{0};
        };
        std::vector<Chunk> chunks;
    };

    /// What a deferred context has recorded since it was created or last
    /// finished; FinishCommandList hands it to the ID3D11CommandList.
    struct DeferredRecording {
        std::vector<RecordedCall> calls;
        GuestArena arena;
        /// The bindings the list starts from: the deferred context's state
        /// when recording began (a FinishCommandList(TRUE) preserves it), held.
        /// Empty means the default state.
        std::optional<ContextBindings> initial;
        /// Scratch for the synthetic call frame of each replayed call.
        GuestAddr replay_frame{0};
    };

    /// A deferred context is its own COM object (Learn,
    /// ID3D11Device::CreateDeferredContext): own reference count, own
    /// private data, own annotation facet, and one reference on its device.
    struct DeferredContext {
        GuestAddr device_owner{0};
        GuestAddr annotation_interface{0};
        u32 ref_count{1};
        u32 context_flags{0};
        PrivateDataStore private_data;
        std::unique_ptr<DeferredRecording> recording;
        /// Per (resource, subresource): the block the last WRITE_DISCARD Map
        /// of this recording handed out. WRITE_NO_OVERWRITE maps the same
        /// block, and without one fails with
        /// D3D11_ERROR_DEFERRED_CONTEXT_MAP_WITHOUT_INITIAL_DISCARD.
        struct Mapping {
            GuestAddr block{0};
            bool mapped{false};
        };
        std::map<std::pair<GuestAddr, u32>, Mapping> mappings;
    };

    struct Query {
        GuestAddr interface_address{0};
        GuestAddr device_owner{0};
        u32 ref_count{1};
        /// The references the application holds (see retain_object).
        u32 public_refs{1};
        u32 query_type{0};
        u32 misc_flags{0};
        /// D3D11.3 20.3: a new query is signaled with no data; Begin starts
        /// building (Begin/End types only); End issues it. GetData on a
        /// never-issued or building query is DXGI_ERROR_INVALID_CALL
        /// (measured).
        enum class State : u8 { NeverIssued, Building, Issued };
        State state{State::NeverIssued};
        /// The context that is building it (State::Building only).
        GuestAddr building_context{0};
        /// A predicate type (query_type_is_predicate): answers
        /// QI(ID3D11Predicate) and may be passed to SetPredication, whichever
        /// of CreateQuery / CreatePredicate made it.
        bool predicate{false};
        /// EVENT / TIMESTAMP / TIMESTAMP_DISJOINT: the command-stream point
        /// the last End recorded. GetData reports S_FALSE until the GPU has
        /// finished the work before it, and a TIMESTAMP reads its GPU time.
        /// OCCLUSION* / PIPELINE_STATISTICS use it only when the backend does
        /// not track submission generations.
        std::shared_ptr<graphics::GpuTimestamp> gpu_point;
        /// Only when the backend cannot time GPU work (no gpu_point).
        u64 host_ticks_at_end{0};
        /// DIAGNOSTIC (transfer log): the frame of the last End, and the
        /// GetData calls since it.
        u64 diag_end_frame{0};
        u32 diag_polls{0};
        /// OCCLUSION* / PIPELINE_STATISTICS: the newest submission generation
        /// holding GPU work the bracket counted (0: none). The data is final
        /// once that generation has completed.
        u64 last_work_generation{0};
        /// The submission generation gpu_point was recorded into (0: not
        /// tracked). GetData flushes only while that one is still open.
        u64 point_generation{0};
        /// ... and whether some of it was recorded where no generation is
        /// tracked, in which case End records gpu_point instead.
        bool untracked_work{false};
        /// Stream-output queries: the stats records (graphics::SO_STATS_WORDS
        /// u32: {needed, written} per stream) of every streaming draw between
        /// Begin and End, written by the GPU accounting and summed once the
        /// bracket's work completed. An overflow predicate is TRUE when
        /// needed > written (for its stream; any stream for the plain one).
        std::vector<std::shared_ptr<graphics::BackendBuffer>> so_stats;
        /// OCCLUSION / OCCLUSION_PREDICATE: the Begin/End intervals of the
        /// context this bracket spans. Each interval is one GPU counter shared
        /// by every bracket open during it, so overlapping brackets each sum
        /// exactly their own draws (D3D11.3 20.3.4).
        std::vector<std::shared_ptr<graphics::GpuOcclusionCounter>>
            occlusion_intervals;
        /// A draw in the bracket could not be counted (no backend counter).
        bool occlusion_unmeasured{false};
        /// PIPELINE_STATISTICS: the 11 UINT64s of
        /// D3D11_QUERY_DATA_PIPELINE_STATISTICS the CPU counted (indexed by
        /// PipelineStatistic, the struct order), a mask of those a draw
        /// could not count (pipeline_statistic_bit), and the argument blocks
        /// of the indirect dispatches in the bracket, copied on the GPU
        /// timeline when they ran and folded in once that work completed.
        std::array<u64, 11> pipeline{};
        u32 pipeline_unmeasured{0};
        struct PendingArguments {
            std::shared_ptr<graphics::BackendBuffer> arguments;
            u64 threads_per_group{0};
        };
        std::vector<PendingArguments> pending_arguments;
        /// The GPU statistics records (graphics::STATS_RECORD_WORDS) of the
        /// intervals this bracket spans: the counters only the GPU knows,
        /// added to the CPU's once the bracket's work completed.
        std::vector<std::shared_ptr<graphics::BackendBuffer>>
            statistics_records;
        /// D3D11_QUERY_DESC1::ContextType (CreateQuery1); 0 = ALL.
        u32 context_type{0};
        PrivateDataStore private_data;
    };

    /// An ID3D11CommandList: a closed recording produced by FinishCommandList.
    struct CommandList {
        GuestAddr interface_address{0};
        GuestAddr device_owner{0};
        u32 ref_count{1};
        /// The references the application holds (see retain_object).
        u32 public_refs{1};
        u32 context_flags{0};
        std::unique_ptr<DeferredRecording> recording;
        PrivateDataStore private_data;
    };

    /// An ID3D11Fence: a guest handle onto a backend timeline.
    struct Fence {
        GuestAddr interface_address{0};
        GuestAddr device_owner{0};
        u32 ref_count{1};
        /// The references the application holds (see retain_object).
        u32 public_refs{1};
        u32 flags{0};
        std::unique_ptr<graphics::BackendFence> backend;
        PrivateDataStore private_data;
    };

    /// An ID3D11ClassLinkage (Learn ID3D11Device::CreateClassLinkage). It
    /// hands out class instances. It numbers every instance name and every
    /// class-type name it sees, in first-seen order, whether a shader
    /// created with it declares the name (its IFCE chunk, in chunk order)
    /// or GetClassInstance / CreateClassInstance asks for it (measured:
    /// g_red 0, g_green 1, g_abstract 2, "nope" 3 before any shader;
    /// d3d11_classlink_probe [desc] for names a shader registered first).
    /// InstanceId and TypeId are those numbers.
    struct ClassLinkage {
        GuestAddr interface_address{0};
        GuestAddr device_owner{0};
        u32 ref_count{1};
        /// The references the application holds (see retain_object).
        u32 public_refs{1};
        std::vector<std::string> instance_names;
        std::vector<std::string> type_names;
        PrivateDataStore private_data;
    };

    /// An ID3D11ClassInstance. GetClassInstance makes a new object on every
    /// call (measured "GetClassInstance twice same object=0") with only the
    /// instance name and ids; CreateClassInstance echoes its arguments with
    /// Created TRUE and only a type name (Learn GetInstanceName /
    /// GetTypeName). It keeps its linkage alive with a runtime reference.
    struct ClassInstance {
        GuestAddr interface_address{0};
        GuestAddr device_owner{0};
        GuestAddr linkage{0};
        u32 ref_count{1};
        /// The references the application holds (see retain_object).
        u32 public_refs{1};
        std::string instance_name;
        std::string type_name;
        /// D3D11_CLASS_INSTANCE_DESC: InstanceId, InstanceIndex, TypeId,
        /// ConstantBuffer, BaseConstantBufferOffset, BaseTexture, BaseSampler,
        /// then the BOOL Created.
        std::array<u32, 7> desc{};
        bool created{false};
        PrivateDataStore private_data;
    };
