// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d9_cs.hpp
/// @brief D3D9-level command stream: T1 records a draw/clear snapshot, a
///        worker builds the BackendDrawCall or GPU clear. T1 opens the pass
///        (that may wait_encoded) before enqueueing a draw.
///
/// This is not the Metal encode thread. The worker must never call
/// MetalSubmitQueue wait_encoded/drain/submit. Clear/ColorFill may EndPass
/// (record only) then record the GPU clear.

#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

namespace vitreo::graphics {
class BackendBuffer;
class BackendPipeline;
class BackendRenderPass;
class BackendSwapChain;
class BackendTexture2D;
class BackendTexture3D;
class GraphicsBackendDevice;
struct Sm3Shader;
}

namespace vitreo::winapi {

/// Copied D3D9 draw inputs plus backends resolved on T1. The worker must not
/// find() COM objects or read live DeviceState.
struct D3d9DrawSnapshot {
    D3D9Manager* manager{nullptr};
    graphics::GraphicsBackendDevice* backend{nullptr};
    graphics::BackendRenderPass* render_pass{nullptr};
    graphics::BackendTexture2D* white_texture{nullptr};
    graphics::BackendTexture2D* rt_color{nullptr};
    std::array<graphics::BackendTexture2D*, 4> rt_colors{};
    std::array<unsigned, 4> rt_color_slices{};
    graphics::BackendTexture2D* rt_depth{nullptr};

    bool indexed{false};
    u32 primitive_type{0};
    u32 first_vertex{0};
    u32 index_start{0};
    u32 primitive_count{0};
    u32 element_count{0};
    i32 base_vertex{0};

    u32 vertex_shader{0};
    u32 pixel_shader{0};
    u32 fvf{0};
    u32 vertex_declaration{0};
    u32 render_target_surface{0};
    std::array<u32, 4> render_target_surfaces{};
    u32 depth_stencil_surface{0};
    u32 indices{0};

    bool has_vs{false};
    bool has_ps{false};
    bool programmable_vs{false};
    bool programmable_ps{false};
    bool programmable_pair{false};
    bool fixed_function_only{false};
    bool auto_depth_stencil{false};
    bool to_back_buffer{false};

    u32 back_buffer_width{0};
    u32 back_buffer_height{0};
    u32 back_buffer_format{0};
    u32 multi_sample_type{0};
    u32 multi_sample_quality{0};
    u32 color_width{0};
    u32 color_height{0};
    u32 color_format{0};
    std::array<u32, 4> color_formats{};

    std::array<u32, 6> viewport{};
    std::array<i32, 4> scissor{};

    std::shared_ptr<const D3d9FrozenDrawTables> tables;
    std::array<std::array<float, 4>, D3D9_MAX_CLIP_PLANES> clip_planes{};
    std::array<u32, D3D9_MAX_SAMPLERS> texture_ids{};
    std::array<u32, D3D9_VERTEX_TEXTURE_SAMPLER_COUNT> vertex_texture_ids{};
    std::array<u8, D3DMATERIAL9_SIZE> material{};

    /// Used programmable constants only (`float_constant_count` / int / bool).
    /// Fixed-function packing reads transforms, material, and lights — not these.
    std::vector<float> vs_const_f;
    std::vector<float> ps_const_f;
    std::vector<i32> vs_const_i;
    std::vector<i32> ps_const_i;
    std::vector<u32> vs_const_b;
    std::vector<u32> ps_const_b;

    /// Borrowed from retained IDirect3D*Shader9 objects. Worker reads only.
    const graphics::Sm3Shader* vs_translated{nullptr};
    const graphics::Sm3Shader* ps_translated{nullptr};
    const u32* ps_bytecode{nullptr};
    usize ps_bytecode_size{0};

    std::vector<D3D9Manager::FvfElement> elements;
    std::array<u32, D3D9_MAX_STREAMS> stream_layout_strides{};
    std::array<bool, D3D9_MAX_STREAMS> stream_used{};

    struct StreamSnap {
        u32 buffer{0};
        u32 offset{0};
        u32 stride{0};
        u32 frequency{1};
        graphics::BackendBuffer* backend{nullptr};
    };
    std::array<StreamSnap, D3D9_MAX_STREAMS> streams{};
    graphics::BackendBuffer* index_backend{nullptr};
    u32 index_format{0};

    struct TextureSnap {
        u32 com_id{0};
        graphics::BackendTexture2D* tex2d{nullptr};
        graphics::BackendTexture3D* tex3d{nullptr};
        u32 width{0};
        u32 height{0};
        bool missing_backing{false};
    };
    std::array<TextureSnap, D3D9_MAX_SAMPLERS> textures{};
    std::array<TextureSnap, D3D9_VERTEX_TEXTURE_SAMPLER_COUNT>
        vertex_textures{};

    std::vector<u32> retained;
    u64 diagnostic_frame{0};
};

/// Copied Clear inputs plus backends resolved on T1. Worker ends an open
/// pass (EndPass only) then records the GPU clear. No wait_encoded.
struct D3d9ClearSnapshot {
    D3D9Manager* manager{nullptr};
    graphics::BackendSwapChain* swap_chain{nullptr};
    graphics::BackendTexture2D* rt_color{nullptr};
    std::array<graphics::BackendTexture2D*, 4> rt_colors{};
    std::array<unsigned, 4> rt_color_slices{};
    std::unique_ptr<graphics::BackendRenderPass> pass_to_end;

    u32 flags{0};
    std::array<float, 4> color{};
    float depth{1.0f};
    u32 stencil{0};
    bool to_back_buffer{false};

    std::vector<u32> retained;
};

/// Full-rect GPU ColorFill. CPU ColorFill stays on T1 (guest pixels).
struct D3d9ColorFillSnapshot {
    D3D9Manager* manager{nullptr};
    graphics::BackendTexture2D* backend{nullptr};
    std::unique_ptr<graphics::BackendRenderPass> pass_to_end;
    std::array<float, 4> color{};
    unsigned mip_level{0};
    unsigned slice{0};
    std::vector<u32> retained;
};

using D3d9CsOp =
    std::variant<D3d9DrawSnapshot, D3d9ClearSnapshot, D3d9ColorFillSnapshot>;

/// Serial D3D9 draw-build thread. One list in flight; T1 records, worker
/// fills BackendDrawCall and calls draw() on a pass T1 already opened.
class D3d9SubmitQueue {
public:
    static D3d9SubmitQueue& instance();

    void enqueue_draw(D3d9DrawSnapshot&& snap);
    void enqueue_clear(D3d9ClearSnapshot&& snap);
    void enqueue_color_fill(D3d9ColorFillSnapshot&& snap);

    /// Wait until recorded draws have been built into the Metal CS.
    /// If T1 is the AppKit main thread, pump the run loop before waiting.
    void flush();

    /// Same wait as flush for this slice (no GPU wait — that stays on Metal).
    void drain();

    void shutdown();

    /// Worker-owned PSO cache. Call only after drain, from T1.
    void clear_pipeline_cache();

    [[nodiscard]] graphics::BackendPipeline* cached_pipeline(u64 key);
    void store_pipeline(u64 key,
                        std::unique_ptr<graphics::BackendPipeline> pipeline);

    /// COM ids the worker finished with; T1 must Release them.
    std::vector<u32> take_completed_retains();

    D3d9SubmitQueue(const D3d9SubmitQueue&) = delete;
    D3d9SubmitQueue& operator=(const D3d9SubmitQueue&) = delete;

private:
    D3d9SubmitQueue();
    ~D3d9SubmitQueue();

    void thread_main();
    void wait_until_idle();
    void enqueue_op(D3d9CsOp&& op);

    std::mutex m_mu;
    std::condition_variable m_cv;
    std::vector<D3d9CsOp> m_queue;
    std::vector<u32> m_completed_retains;
    std::unordered_map<u64, std::unique_ptr<graphics::BackendPipeline>>
        m_pipelines;
    std::thread m_thread;
    std::atomic<bool> m_running{true};
    bool m_busy{false};
    u64 m_submit_gen{0};
    u64 m_done_gen{0};
};

} // namespace vitreo::winapi
