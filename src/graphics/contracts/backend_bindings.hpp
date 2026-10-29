// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file backend_bindings.hpp
/// @brief The shared Metal binding namespace and descriptor tables.

#include "graphics/contracts/backend_pipeline.hpp"

namespace vitreo::graphics {

// =============================================================================
// Shared binding namespace
// =============================================================================
//
// D3D exposes b# (constant buffers), t# (shader resources), u# (unordered
// access) and the input-assembler streams as four independent register
// namespaces. Metal collapses buffers into ONE index namespace per stage and
// textures into another, so the four D3D namespaces must be given disjoint
// windows inside those two.
//
// This layout is a contract, not an implementation detail. Three separate
// layers encode it and every one of them must agree:
//
//   * the shader translator, which tells SPIRV-Cross which MSL index each
//     SPIR-V binding compiles to (src/graphics/shaders/spirv_to_msl.cpp)
//   * the backend, which binds resources at those indices
//     (src/graphics/metal/metal_device.mm)
//   * every frontend that fills BackendDrawCall / BackendDispatchCall
//     (e.g. D3D11Manager::execute_dispatch)
//
// They used to encode it as four unrelated magic numbers with comments asking
// the next reader to keep them in step by hand. They are derived here once
// instead, because a disagreement between them is a silent wrong-render: the
// shader samples an index nothing was bound at, and the draw succeeds.

/// t# / u# register capacity. A t# slot names a texture, a structured buffer,
/// or a typed buffer — never more than one — so the three share this numbering.
constexpr unsigned BACKEND_MAX_TEXTURE_SLOTS = 8;
/// b# register capacity.
constexpr unsigned BACKEND_MAX_CONSTANT_BUFFER_SLOTS = 4;
/// Input-assembler stream capacity.
constexpr unsigned BACKEND_MAX_VERTEX_BUFFERS = 8;

/// Buffer index 0: D3D9 inline shader constants.
constexpr unsigned BACKEND_INLINE_CONSTANT_BUFFER_INDEX = 0;
/// Buffer index 1: the legacy single-stream vertex data binding.
constexpr unsigned BACKEND_LEGACY_VERTEX_DATA_INDEX = 1;
/// Buffer window for b0..b{N-1}.
constexpr unsigned BACKEND_CONSTANT_BUFFER_BASE = 2;
/// Buffer window for structured/raw SRVs t0..t{N-1}.
constexpr unsigned BACKEND_SRV_BUFFER_BASE =
    BACKEND_CONSTANT_BUFFER_BASE + BACKEND_MAX_CONSTANT_BUFFER_SLOTS;
/// Buffer window for structured/raw UAVs u0..u{N-1}.
constexpr unsigned BACKEND_UAV_BUFFER_BASE =
    BACKEND_SRV_BUFFER_BASE + BACKEND_MAX_TEXTURE_SLOTS;
/// Buffer window for the input-assembler streams.
///
/// This sits ABOVE the u# window on purpose. It used to start at 16, which
/// overlapped u2..u7 (14..21). That was survivable only because UAV buffers
/// were bound on the compute stage and vertex buffers on the vertex stage, and
/// Metal's index namespace is per-stage. A vertex-stage UAV is legal D3D11 and
/// would have aliased a vertex buffer.
constexpr unsigned BACKEND_VERTEX_BUFFER_BASE =
    BACKEND_UAV_BUFFER_BASE + BACKEND_MAX_TEXTURE_SLOTS;
/// The vertex-stage capture buffer: where pass A of the geometry emulation
/// (.docs/design-geometry-shader-emulation.md) writes every output register of
/// every vertex, in output-signature order, keyed by vertex index.
///
/// Both consumers of vertex-stage output that Metal lacks go through it. A
/// geometry shader reads it as `gs_in` in the compute expansion; stream-out
/// (`CreateGeometryShaderWithStreamOutput`) repacks it, or the expansion's
/// `gs_out`, into the guest's SO targets in a compute pass. So the vertex stage
/// needs exactly one extra index however many SO targets are bound, and the
/// four targets and the filled-size counter live in the compute stage, whose
/// input-assembler window is free.
///
/// It must be disjoint from the vertex-buffer window. The capture draw is a
/// real draw with the guest's input layout and vertex buffers bound -- that is
/// what makes hardware vertex fetch work unchanged -- so the compute stage's
/// habit of reusing that window for set 4 does not carry over. The translator
/// once gave the vertex stage the compute mapping and the capture landed on
/// vertex buffer 0; nothing failed, the draw simply fetched its own output.
///
/// It is the last index below Metal's per-stage limit of 31.
constexpr unsigned BACKEND_VERTEX_CAPTURE_BUFFER_INDEX =
    BACKEND_VERTEX_BUFFER_BASE + BACKEND_MAX_VERTEX_BUFFERS;

/// D3D11_SO_BUFFER_SLOT_COUNT.
constexpr unsigned BACKEND_MAX_STREAM_OUT_TARGETS = 4;

/// One past the highest buffer index any stage can bind.
constexpr unsigned BACKEND_BUFFER_INDEX_LIMIT =
    BACKEND_VERTEX_CAPTURE_BUFFER_INDEX + 1;

/// Texture window for SRVs t0..t{N-1}.
constexpr unsigned BACKEND_SRV_TEXTURE_BASE = 0;
/// Texture window for UAVs u0..u{N-1}.
constexpr unsigned BACKEND_UAV_TEXTURE_BASE =
    BACKEND_SRV_TEXTURE_BASE + BACKEND_MAX_TEXTURE_SLOTS;
/// One past the highest texture index any stage can bind.
constexpr unsigned BACKEND_TEXTURE_INDEX_LIMIT =
    BACKEND_UAV_TEXTURE_BASE + BACKEND_MAX_TEXTURE_SLOTS;

/// Sampler index where s#'s *border twin* binds: the same sampler with an
/// opaque-white border where s# itself binds with a transparent-black one.
///
/// D3D11 takes any float4 as a border colour; Metal offers three. Sampling
/// once through each and blending, `lo + (hi - lo) * border`, recovers any
/// colour exactly: `hi - lo` is precisely the weight the filter gave the
/// border, whatever the filter (the DXBC translator's border_twin_samplers).
/// The flat s# window is 0..7, so the twins take 8..15 -- the last of Metal's
/// 16 sampler indices.
constexpr unsigned BACKEND_BORDER_TWIN_SAMPLER_BASE = BACKEND_MAX_TEXTURE_SLOTS;
constexpr unsigned BACKEND_METAL_SAMPLER_ARGUMENT_LIMIT = 16;
static_assert(BACKEND_BORDER_TWIN_SAMPLER_BASE + BACKEND_MAX_TEXTURE_SLOTS <=
                  BACKEND_METAL_SAMPLER_ARGUMENT_LIMIT,
              "border twins must fit Metal's 16 sampler indices");

// Metal's per-stage argument limits. Raising a capacity above these does not
// fail at run time in any visible way — the excess bindings are simply dropped
// by the encoder — so the ceiling is asserted here instead.
//
// The buffer ceiling is why reaching D3D11's full b0..b13 needs argument
// buffers rather than a larger window: 14 constant buffers would push
// BACKEND_BUFFER_INDEX_LIMIT to 40 and trip this assert.
constexpr unsigned BACKEND_METAL_BUFFER_ARGUMENT_LIMIT = 31;
constexpr unsigned BACKEND_METAL_TEXTURE_ARGUMENT_LIMIT = 128;

// If any window below grows, this fires rather than letting the encoder drop
// the capture binding -- a capture draw that writes nothing looks exactly like a
// shader that output nothing.
static_assert(BACKEND_VERTEX_CAPTURE_BUFFER_INDEX >=
                  BACKEND_VERTEX_BUFFER_BASE + BACKEND_MAX_VERTEX_BUFFERS,
              "the vertex capture buffer overlaps the vertex-buffer window");
static_assert(BACKEND_VERTEX_CAPTURE_BUFFER_INDEX <
                  BACKEND_METAL_BUFFER_ARGUMENT_LIMIT,
              "the vertex capture buffer is past Metal's per-stage buffer "
              "limit");

static_assert(BACKEND_BUFFER_INDEX_LIMIT <=
                  BACKEND_METAL_BUFFER_ARGUMENT_LIMIT,
              "buffer binding windows exceed Metal's per-stage buffer "
              "argument limit; use argument buffers instead of widening them");
static_assert(BACKEND_TEXTURE_INDEX_LIMIT <=
                  BACKEND_METAL_TEXTURE_ARGUMENT_LIMIT,
              "texture binding windows exceed Metal's per-stage texture "
              "argument limit");

/// Buffer window for the geometry-shader expansion buffers: gs_in, gs_out,
/// gs_counter and the index buffer the expansion gathers through.
///
/// These reuse the input-assembler window's indices, which is safe for a
/// reason worth stating rather than assuming: they are bound ONLY by the
/// compute expansion pass, and the compute stage has no input-assembler
/// streams. The caution attached to BACKEND_VERTEX_BUFFER_BASE above -- that
/// per-stage disjointness is not a safe thing to rely on -- applies to
/// bindings a GUEST shader can request at both. No guest shader declares
/// descriptor set 4; the translator emits it only when lowering a geometry
/// shader, which always runs as this pass, and in the pass-A capture vertex
/// shader, which the MSL binder maps to BACKEND_VERTEX_CAPTURE_BUFFER_INDEX
/// instead because that stage DOES have input-assembler streams.
constexpr unsigned BACKEND_GS_EXPANSION_BUFFER_BASE =
    BACKEND_VERTEX_BUFFER_BASE;
constexpr unsigned BACKEND_MAX_GS_EXPANSION_BUFFERS = 4;

/// Buffer window for the hidden counters of APPEND/COUNTER UAVs, which a
/// shader moves with `imm_atomic_alloc` / `imm_atomic_consume`.
///
/// Indexed by the ORDER the shader uses its counters in
/// (DxbcToSpirvResult::uav_counter_registers), not by u# number: the indices
/// left below Metal's limit are five, and D3D11.0 has eight u#. A shader using
/// more than five counters fails translation, loudly.
///
/// Compute and pixel stages only. In compute it sits above the geometry
/// expansion window. In the fragment stage it sits inside the input-assembler
/// window, which only the vertex stage binds; the capture buffer at 30 is
/// vertex-only too. The vertex stage has no free index at all, so the
/// translator refuses a counter in any other stage.
constexpr unsigned BACKEND_UAV_COUNTER_BUFFER_BASE =
    BACKEND_GS_EXPANSION_BUFFER_BASE + BACKEND_MAX_GS_EXPANSION_BUFFERS;
constexpr unsigned BACKEND_MAX_UAV_COUNTER_BINDINGS =
    BACKEND_METAL_BUFFER_ARGUMENT_LIMIT - BACKEND_UAV_COUNTER_BUFFER_BASE;
static_assert(BACKEND_UAV_COUNTER_BUFFER_BASE >= BACKEND_VERTEX_BUFFER_BASE,
              "fragment-stage counters must not overlap a fragment binding");

/// The fragment stage's PIPELINE_STATISTICS record, which a pixel shader
/// translated with count_pixel_invocations adds its invocations to
/// (graphics/shaders/pipeline_stats.hpp). Fragment only, in the
/// input-assembler window the fragment stage does not use, below the UAV
/// counters; the compute stage's expansion buffers reuse the index, which is
/// safe because no compute kernel reads the record through it.
constexpr unsigned BACKEND_FRAGMENT_STATISTICS_BUFFER_INDEX =
    BACKEND_GS_EXPANSION_BUFFER_BASE;
static_assert(BACKEND_FRAGMENT_STATISTICS_BUFFER_INDEX <
                  BACKEND_UAV_COUNTER_BUFFER_BASE,
              "the statistics record must not overlap the UAV counters");

/// Compute buffer bindings. The compute stage has no input-assembler streams,
/// so its array reuses that range for the geometry-expansion buffers, then the
/// UAV counters, and reaches Metal's limit.
constexpr unsigned BACKEND_MAX_COMPUTE_BUFFERS =
    BACKEND_UAV_COUNTER_BUFFER_BASE + BACKEND_MAX_UAV_COUNTER_BINDINGS;

static_assert(BACKEND_MAX_COMPUTE_BUFFERS <=
                  BACKEND_METAL_BUFFER_ARGUMENT_LIMIT,
              "compute buffer window exceeds Metal's per-stage buffer "
              "argument limit");
/// Compute texture bindings: the same disjoint t#/u# split as the draw stages.
constexpr unsigned BACKEND_MAX_COMPUTE_TEXTURES = BACKEND_TEXTURE_INDEX_LIMIT;

// =============================================================================
// Descriptor tables
// =============================================================================
//
// The flat windows above cannot hold the D3D contract. Per stage D3D11 exposes
// 14 constant buffers, 128 shader resources and 64 unordered-access views, and
// D3D12 exposes descriptor heaps with thousands of entries. Metal allows 31
// buffer arguments per stage in total, shared with the input-assembler streams.
//
// A descriptor table is the way out, and it is the same object in both APIs: a
// GPU-addressable buffer of resource descriptors (a Metal Tier-2 argument
// buffer), indexed by the shader. One buffer argument holds a whole register
// space.
//
// It also fixes a CPU-side problem. `BackendDrawCall` carries its bindings by
// value, so widening a per-slot array multiplies a struct that is built on every
// draw. A table is owned by the frontend, updated only when a binding changes —
// which is D3D's model, where `PSSetShaderResources` leaves the rest of the t#
// space alone — and referenced by the draw as one pointer.
//
// Two rules make the layout a contract rather than a coincidence:
//
//  * Slot N of a table is descriptor N of its descriptor set. The shader
//    compiler pins this with `[[id(N)]]` (see MSL_DESCRIPTOR_SLOT_BYTES and
//    `spirv_to_msl.hpp`), so the host encodes absolute D3D register numbers.
//  * A draw either supplies descriptor tables or flat bindings, never a mix.
//    Both would want the same Metal buffer indices, so the backend honours
//    whichever the call provides and ignores the other.

/// What a table's slots hold. Every class encodes to the same 8 bytes; this
/// selects how the shader declares an unused slot, and nothing else.
enum class BackendDescriptorClass : u8 { Texture, Buffer, Sampler };

/// Descriptor sets, one per D3D register space: 0 = b#, 1 = t#, 2 = s#, 3 = u#.
constexpr unsigned BACKEND_MAX_DESCRIPTOR_SETS = 4;
/// The u# set: the only one whose resources a shader can write. b#, t# and s#
/// are read-only views by D3D's own contract.
constexpr unsigned BACKEND_DESCRIPTOR_SET_UAV = 3;
static_assert(BACKEND_DESCRIPTOR_SET_UAV < BACKEND_MAX_DESCRIPTOR_SETS);

/// Metal buffer index descriptor set `n` binds its argument buffer at.
///
/// Deliberately the same window as the flat b# bindings. That is safe only
/// because of the no-mixing rule above: a draw that supplies tables never also
/// supplies flat constant buffers. The alternative — a disjoint window — does
/// not exist, because the flat windows plus the input-assembler streams already
/// reach index 29 of the 31 Metal allows.
constexpr unsigned BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE =
    BACKEND_CONSTANT_BUFFER_BASE;

static_assert(BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE +
                      BACKEND_MAX_DESCRIPTOR_SETS <=
                  BACKEND_VERTEX_BUFFER_BASE,
              "descriptor-table argument buffers must not collide with the "
              "input-assembler streams, which both binding modes need");

struct BackendDescriptorTableDesc {
    /// Register count this table covers. Slot N is register N.
    unsigned slot_count{0};
    BackendDescriptorClass slot_class{BackendDescriptorClass::Texture};
};

/// A persistent, GPU-addressable array of resource descriptors.
///
/// Writes are persistent: a slot holds its value until overwritten or cleared,
/// so a frontend mirrors its binding state here instead of rebuilding it per
/// draw. A draw takes a snapshot, so a write made after the draw was submitted
/// cannot reach back and change it.
class BackendDescriptorTable {
public:
    virtual ~BackendDescriptorTable() = default;

    [[nodiscard]] virtual bool valid() const noexcept = 0;
    [[nodiscard]] virtual unsigned slot_count() const noexcept = 0;

    /// Bind a 2D texture, honouring @p view's mip/slice/format window exactly as
    /// the flat path does. A null texture clears the slot.
    [[nodiscard]] virtual bool set_texture(
        unsigned slot, const BackendTexture2D* texture,
        const BackendTextureViewDesc& view) = 0;
    [[nodiscard]] virtual bool set_texture_3d(unsigned slot,
                                              const BackendTexture3D* texture) = 0;
    /// Bind a buffer at @p offset bytes. A null buffer clears the slot.
    [[nodiscard]] virtual bool set_buffer(unsigned slot,
                                          const BackendBuffer* buffer,
                                          unsigned offset) = 0;
    /// Bind a typed buffer view (D3D `Buffer<T>` SRV / `RWBuffer<T>` UAV): a
    /// texture-buffer over the view's element window, read and written with
    /// its format's conversion exactly as the flat path binds it. A null view
    /// clears the slot.
    [[nodiscard]] virtual bool set_texture_buffer(
        unsigned slot, const BackendTextureBuffer* view) = 0;
    [[nodiscard]] virtual bool set_sampler(unsigned slot,
                                           const BackendSamplerDesc& sampler) = 0;
    /// Leave the slot holding no resource. A shader that reads it reads nothing;
    /// it is not an error, because D3D lets a shader declare registers it does
    /// not use and lets a caller unbind ones it no longer needs.
    virtual void clear_slot(unsigned slot) = 0;
};

} // namespace vitreo::graphics
