// SPDX-License-Identifier: LGPL-2.1-only
// The context a raytracing-pipeline function copy is lowered in
// (dxil_raytracing.hpp, dxil_lower_rtpipe.inc). Included in dxil_lower.cpp's
// anonymous namespace before the Lowerer class; not a standalone header.

/// What a function copy is in the megakernel.
enum class RtRole : u8 {
    Raygen, Miss, ClosestTri, ClosestProc, AnyTri, AnyProc, Intersection, Callable,
};

/// One recursion level's registers: frame.x the current shader record's
/// byte offset in its table, .y the any-hit verdict (1 ignore, 2 accept
/// and end search), .z the ray's TMax, .w a reported hit's T; frame2.x the
/// reported HitKind, .y the committed one. Its x# arrays: the payload, the
/// candidate attributes (a triangle's barycentrics or a reported hit's),
/// the committed procedural attributes, a callable's parameter.
struct RtLevel {
    u32 frame{0};
    u32 frame2{0};
    u32 payload{0};
    u32 attr{0};
    u32 attrc{0};
    u32 call{0};
};

struct RtHitGroup {
    u32 index{0};
    bool procedural{false};
    bool any{false};
    bool closest{false};
    bool intersection{false};
};

struct RtLocal {
    u32 space{0};
    u32 reg{0};
    u32 record_offset{0};
    u32 words{0};
};

/// The SM 6.6 heap-indexed resources of a whole megakernel: every copy
/// adds to one list (heap_resource), whose ids follow every declared one.
struct RtHeapResources {
    std::deque<Resource> list;
    std::array<u32, 4> base{};
};

struct RtFunctionContext {
    RtRole role{RtRole::Raygen};
    u32 level{0};
    /// TraceRay runs below max_trace, CallShader below max_level.
    u32 max_trace{0};
    u32 max_level{0};
    /// The hit group a hit-group role belongs to.
    u32 hit_group{0};
    const std::vector<RtLevel>* levels{nullptr};
    /// The label of (identifier index, role, level), ~0u when none.
    std::function<u32(u32, RtRole, u32)> label_of;
    const std::vector<RtHitGroup>* hit_groups{nullptr};
    std::vector<u32> miss_indices;
    std::vector<u32> callable_indices;
    /// Range IDs of the reserved resources: the raygen, miss, hit and
    /// callable tables (t0..t3) and the dispatch constants (b0).
    u32 table_ids[4]{};
    u32 const_id{0};
    /// Acceleration-structure SRV range ID -> its companion's range ID.
    const std::map<u32, u32>* companions{nullptr};
    u32 pipeline_flags{0};
    u32 payload_words{1};
    u32 attr_words{2};
    u32 call_words{1};
    /// The local root-signature constants of this copy's export.
    std::vector<RtLocal> local;
    /// This copy's local root signature (null when none), and what its
    /// other arguments read: (class, range id) of a library resource -> its
    /// reserved view array k; the arrays; the reserved static samplers by
    /// n; the translation buffer's range id and words per record.
    const DxilRtProgramDesc::Root* local_root{nullptr};
    const std::map<std::pair<u32, u32>, u32>* view_arrays{nullptr};
    const std::vector<Resource>* views{nullptr};
    const std::map<u32, Resource>* samplers{nullptr};
    u32 translation_id{~0u};
    u32 view_stride{0};
    [[nodiscard]] u32 table() const {
        switch (role) {
        case RtRole::Raygen: return 0;
        case RtRole::Miss: return 1;
        case RtRole::Callable: return 3;
        default: return 2;
        }
    }
};

struct DxilRtBuilder;
