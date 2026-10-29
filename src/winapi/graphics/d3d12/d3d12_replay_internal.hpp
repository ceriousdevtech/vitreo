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

namespace vitreo::winapi::d3d12 {

namespace {

constexpr u64 CONSTANT_CHUNK_BYTES = 256u * 1024u;
constexpr u32 STAGE_VERTEX = 0;
constexpr u32 STAGE_PIXEL = 1;
constexpr u32 STAGE_COMPUTE = 2;

/// Register class of descriptor set N.
constexpr u32 SET_CBV = 0;
constexpr u32 SET_SRV = 1;
constexpr u32 SET_SAMPLER = 2;
constexpr u32 SET_UAV = 3;

[[nodiscard]] inline bool is_d24_family(u32 format) {
    return format >= DXGI_FORMAT_R24G8_TYPELESS &&
           format <= DXGI_FORMAT_X24_TYPELESS_G8_UINT;
}

}  // namespace

/// Backend objects the replay keeps between lists.
struct ReplayResources {
    /// Descriptor tables per stage (VS, PS, CS) and set. The backend
    /// snapshots a table when a draw is recorded, so one table per stage and
    /// set is rewritten for every draw.
    std::array<std::array<std::unique_ptr<graphics::BackendDescriptorTable>, 4>,
               3>
        tables;
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
};

/// The state a list replays with.
struct ReplayState {
    PipelineState* pso{nullptr};
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
    /// SetPredication's verdict: the predicated operations are skipped.
    bool predicated_off{false};
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

    __attribute__((internal_linkage))
    graphics::BackendDescriptorTable* table(u32 stage, u32 set, u32 slots,
                                            graphics::BackendDescriptorClass klass);
    __attribute__((internal_linkage))
    bool upload(const void* data, usize bytes, graphics::BackendBuffer*& buffer,
                u64& offset);
    Resource* resource(GuestAddr address, u64 serial) {
        return f.find_serial<Resource>(address, serial, Kind::Resource);
    }
    /// Bind @p d; returns the view's size in 32-bit words for a raw or
    /// structured buffer view (the push block's size table), else 0.
    __attribute__((internal_linkage))
    u32 bind_view(graphics::BackendDescriptorTable& t, unsigned slot,
                  const Descriptor& d, bool uav);
    bool bind_stage(ReplayState& st, u32 stage, const CompiledShader& shader,
                    std::array<const graphics::BackendDescriptorTable*, 4>& out,
                    std::vector<u32>& push);
    /// @p indirect: the counts come from that GPU buffer (ExecuteIndirect).
    void draw(ReplayState& st, bool indexed, const cmd::DrawIndexed& d,
              const graphics::BackendIndirectArgs* indirect = nullptr);
    void dispatch(ReplayState& st, const cmd::Dispatch& d,
                  const graphics::BackendIndirectArgs* indirect = nullptr);
    /// CPU-visible upload space: @p bytes at @p host, which the GPU reads
    /// at (@p buffer, @p offset).
    bool reserve(usize bytes, graphics::BackendBuffer*& buffer, u64& offset,
                 u8*& host);
    /// @p bytes of @p r at @p offset as the GPU leaves them after all work
    /// recorded so far (submits and waits).
    bool gpu_read(Resource& r, u64 offset, void* out, usize bytes);
    void query(ReplayState& st, const cmd::Query& c);
    void resolve_query(const cmd::ResolveQuery& c);
    void set_predication(ReplayState& st, const cmd::SetPredication& c);
    __attribute__((internal_linkage))
    void execute_indirect(ReplayState& st, const cmd::ExecuteIndirect& c);
    __attribute__((internal_linkage))
    void write_immediate(const cmd::WriteImmediate& c);
    void copy_texture_region(const cmd::CopyTextureRegion& c);
    void resolve(Resource& dst, Resource& src, const cmd::ResolveSubresource& c);
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
};

}  // namespace vitreo::winapi::d3d12
