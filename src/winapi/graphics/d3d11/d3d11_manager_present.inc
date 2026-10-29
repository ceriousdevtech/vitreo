// SPDX-License-Identifier: LGPL-2.1-only
// @file d3d11_manager_present.inc
// @brief D3D11Manager class body, part 10 of 17: swap-chain release and
//   presentation (exclusive full screen, diagnostic dumps, present, the
//   foreign-surface queue).
//
// A fragment of the D3D11Manager definition in d3d11.hpp, which
// includes it inside the class, in this order; not a standalone header.

    [[nodiscard]] u32 release_swap_chain(GuestAddr address);
    void destroy_swap_chain_if_unreferenced(GuestAddr address);
    /// IDXGISwapChain::SetFullscreenState(TRUE, pTarget) and the
    /// Windowed=FALSE creation path: choose the output and mode, publish the
    /// mode through the shared exclusive owner and enter the host Space.
    /// Returns S_OK or the documented failure.
    [[nodiscard]] u32 enter_exclusive_fullscreen(GuestAddr self,
                                                 GuestAddr target);
    /// Relinquish exclusive full-screen: restore the desktop mode and the
    /// window rectangle; @p leave_host_space also leaves the macOS Space.
    void leave_exclusive_fullscreen(GuestAddr self, bool leave_host_space);
    /// IDXGISwapChain::ResizeTarget on a full-screen chain: switch the
    /// output to the supported mode closest to width x height.
    [[nodiscard]] u32 resize_exclusive_target(GuestAddr self, u32 width,
                                              u32 height);
    /// DXGI relinquishes full-screen when the application loses focus
    /// (another application becomes foreground). Observed at the chain's next
    /// DXGI call.
    void relinquish_fullscreen_if_inactive(GuestAddr self);
    /// Present the guest back-buffer Texture2D via BackendSwapChain::
    /// present_texture when available (D3D11 Clear/Draw target).
    /// Diagnostic render-target dump on /tmp/vitreo-rt-dump (d3d11_rt_dump.cpp).
    void dump_render_targets_if_requested();
    /// DIAGNOSTIC: count a draw by @p state into @p texture's
    /// Texture2D::diag_draw_pairs. Callers test m_diag_draw_pairs_armed
    /// first, so a run that never asks for a dump pays one branch per
    /// render target per draw.
    void note_diag_draw_pair(Texture2D& texture, const ContextState& state);
    /// Set by the first render-target dump: from then on each target keeps
    /// the draws since the previous dump (d3d11_rt_dump.cpp).
    bool m_diag_draw_pairs_armed{false};
    /// DIAGNOSTIC (d3d11_rt_dump.cpp): the transfer log, opened at the
    /// start of the first render-target dump as <dump dir>/transfers.txt --
    /// or stderr (the Vitreo log) when that cannot be opened, the reason in
    /// stats.txt -- and null until then, so a run that never asks for a dump
    /// pays one branch per call below. Every line is
    /// "[d3d11-xfer] frame=<presents> <event> ..." (diag_xfer); the events
    /// trace a GPU -> CPU readback end to end: copies into STAGING, EVENT
    /// query End / GetData, Map of STAGING, UAV clears of buffers, the OM u#
    /// binding, and copies / updates into textures larger than 4096 x 4096.
    /// Observation only; bounded by kDiagTransferLines.
    std::FILE* m_diag_transfer_log{nullptr};
    bool m_diag_transfer_to_stderr{false};
    u64 m_diag_transfer_lines{0};
    static constexpr u64 kDiagTransferLines = 500000;
    /// Presents so far (present_swap_chain_frame): the transfer log's frame.
    u64 m_diag_frame{0};
    /// Open the transfer log for the dump in @p dir (no-op once open);
    /// says where it went, or why not, in @p stats.
    void open_diag_transfer_log(const std::string& dir, std::FILE* stats);
    /// One transfer-log line: "[d3d11-xfer] frame=<n> " and @p format.
    void diag_xfer(const char* format, ...)
        __attribute__((format(printf, 2, 3)));
    /// A CopySubresourceRegion (@p src non-zero; @p src_box the guest
    /// D3D11_BOX or 0) or UpdateSubresource (@p dst_box the guest D3D11_BOX or
    /// 0, @p row_pitch / @p depth_pitch the source pitches) into @p dst:
    /// logged when @p dst is a texture larger than 4096 x 4096. @p result is
    /// the copy's outcome ("ok" or why it was refused); an update is logged
    /// as it is called ("called", or "no data" for a NULL pSrcData).
    void diag_log_large_texture_write(const char* what, GuestAddr dst,
                                      u32 dst_sub, u32 dst_x, u32 dst_y,
                                      u32 dst_z, GuestAddr dst_box,
                                      GuestAddr src, u32 src_sub,
                                      GuestAddr src_box, u32 row_pitch,
                                      u32 depth_pitch, const char* result);
    /// A CopyResource / CopySubresourceRegion whose destination is a
    /// STAGING resource (logged at the call, before validation).
    void diag_log_staging_copy(const char* op, GuestAddr dst, u32 dst_sub,
                               u32 dst_x, u32 dst_y, u32 dst_z, GuestAddr src,
                               u32 src_sub, GuestAddr src_box);
    /// An EVENT query's End (@p end) or GetData (@p flags, @p data_size,
    /// @p hr).
    void diag_log_event_query(GuestAddr query, bool end, u32 flags,
                              u32 data_size, u32 hr);
    /// Any Map of a STAGING resource that returned @p hr, with the first 64
    /// bytes at @p data on S_OK and the frames since its last GPU copy.
    void diag_log_staging_map(GuestAddr resource, u32 subresource,
                              u32 map_type, u32 map_flags, u32 hr,
                              GuestAddr data, u32 row_pitch);
    /// ClearUnorderedAccessViewUint / Float (@p is_float) of a buffer view.
    void diag_log_clear_uav(GuestAddr view, bool is_float,
                            const std::array<u32, 4>& values, bool cleared);
    /// OMSetRenderTargetsAndUnorderedAccessViews: the u# half as called.
    void diag_log_om_uavs(u32 rtvs, GuestAddr dsv, u32 uav_start,
                          u32 uav_count, GuestAddr uavs,
                          GuestAddr initial_counts);
    /// IDXGISwapChain::Present / Present1: validation, TEST, DO_NOT_WAIT and
    /// the present itself. Returns the HRESULT.
    [[nodiscard]] u32 present_swap_chain(GuestAddr address,
                                         u32 sync_interval, u32 flags);
    /// Show the current back buffer (no validation, no counting).
    [[nodiscard]] bool present_swap_chain_frame(GuestAddr address,
                                                u32 sync_interval, u32 flags,
                                                bool rotate);
    void enqueue_foreign_surface_present(
        u64 hwnd, u64 shared_handle, u32 width, u32 height, u32 sync_interval,
        u32 src_x = 0, u32 src_y = 0);
    void wait_for_foreign_surface_reuse(u64 shared_handle);
    void run_foreign_present_worker(std::stop_token stop);
    /// Rotate the buffer ring of a flip / SEQUENTIAL chain one step: buffer
    /// 1 becomes buffer 0 and the presented frame moves to the end.
    [[nodiscard]] bool advance_flip_back_buffer(SwapChain& swap_chain,
                                                Texture2D& texture);
    void release_device_reference(GuestAddr owner);
    void unsupported(Marshaler& m, const char* interface_name, usize slot);
