// SPDX-License-Identifier: LGPL-2.1-only
/// @file filter_graph_video.cpp
/// @brief Host video presentation for a filter graph and the IVideoWindow /
/// IBasicVideo dispatchers.

#include "filter_graph_internal.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"

#include <cstring>
#include <vector>

namespace vitreo::winapi {

void ComCoreManager::present_filter_graph_video(FilterGraphState& st,
                                               Marshaler* m) {
    // A guest renderer gets its frames from the graph's streaming thread,
    // never from the application thread that happens to call in here.
    (void)m;
    if (st.guest_video_connected) return;
    if (m_cocoa_bridge == nullptr || st.player == nullptr ||
        !st.video_renderer_connected) {
        return;
    }
    const bool auto_show = st.video_auto_show != 0;
    const bool visible = st.video_visible != 0;
    if (!visible && !auto_show) return;
    if (st.filter_state == 0) {
        if (!st.video_overlay_logged) {
            st.video_overlay_logged = true;
            std::fprintf(stderr,
                         "[ole32] video overlay skip: graph stopped\n");
        }
        return;
    }

    std::vector<u8> rgba;
    u32 width = 0;
    u32 height = 0;
    u64 frame_index = 0;
    if (!st.player->copy_video_frame(rgba, width, height, frame_index)) {
        if (!st.video_overlay_logged) {
            st.video_overlay_logged = true;
            std::fprintf(stderr,
                         "[ole32] video overlay skip: no decoded frame yet "
                         "state=%u\n",
                         st.filter_state);
        }
        return;
    }
    if (frame_index == st.last_video_frame && st.video_overlay_hwnd != 0) {
        return;
    }

    windowing::HWND hwnd = static_cast<windowing::HWND>(st.video_owner);
    if (hwnd == 0) hwnd = m_cocoa_bridge->active_hwnd();
    if (hwnd == 0) return;
    if (st.video_overlay_hwnd != 0 && st.video_overlay_hwnd != hwnd) {
        m_cocoa_bridge->hide_video_overlay(
            static_cast<windowing::HWND>(st.video_overlay_hwnd));
    }
    const i32 dest_l = st.video_position_set ? st.video_left : 0;
    const i32 dest_t = st.video_position_set ? st.video_top : 0;
    const i32 dest_w = st.video_position_set ? st.video_width : 0;
    const i32 dest_h = st.video_position_set ? st.video_height : 0;
    m_cocoa_bridge->present_video_overlay(
        hwnd, rgba.data(), static_cast<i32>(width), static_cast<i32>(height),
        dest_l, dest_t, dest_w, dest_h);
    if (st.video_overlay_hwnd == 0) {
        std::fprintf(stderr,
                     "[ole32] video overlay present hwnd=0x%x %ux%u "
                     "frame=%llu state=%u\n",
                     hwnd, width, height,
                     static_cast<unsigned long long>(frame_index),
                     st.filter_state);
    }
    st.video_overlay_hwnd = hwnd;
    st.last_video_frame = frame_index;
    st.video_overlay_logged = true;
}

void ComCoreManager::hide_filter_graph_video(FilterGraphState& st) {
    if (m_cocoa_bridge == nullptr) return;
    if (st.video_overlay_hwnd != 0) {
        std::fprintf(stderr, "[ole32] video overlay hide hwnd=0x%x\n",
                     st.video_overlay_hwnd);
        m_cocoa_bridge->hide_video_overlay(
            static_cast<windowing::HWND>(st.video_overlay_hwnd));
        st.video_overlay_hwnd = 0;
    }
    st.last_video_frame = ~u64{0};
}

u32 ComCoreManager::filter_graph_video_window(Marshaler& m, u32 op) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) return com::E_INVALIDARG;
    std::fprintf(stderr, "[ole32] IVideoWindow op=%u\n", op);
    // Learn IVideoWindow: Filter Graph Manager exposes the interface before
    // the graph is built; methods return E_NOINTERFACE until a video renderer
    // is present.
    if (!st->video_renderer_connected) return E_NOINTERFACE;

    const auto write_i32 = [&](GuestAddr ptr, i32 value) -> u32 {
        if (ptr == 0) return E_POINTER;
        (void)m.write_u32(ptr, static_cast<u32>(value));
        return com::S_OK;
    };
    const auto put_i32 = [&](i32& field) -> u32 {
        field = static_cast<i32>(m.arg32(1));
        return com::S_OK;
    };
    // OAHWND is a LONG_PTR: pointer-width in and out.
    const auto arg_oahwnd = [&](usize index) -> u64 {
        return m.cpu_is_64bit() ? m.arg(index) : (m.arg(index) & 0xFFFFFFFFull);
    };
    const auto write_oahwnd = [&](GuestAddr ptr, u64 value) -> u32 {
        if (ptr == 0) return E_POINTER;
        (void)m.write_ptr(ptr, value);
        return com::S_OK;
    };

    switch (op) {
    case 0: // put_Caption
        return com::S_OK;
    case 1: // get_Caption(BSTR*) — NULL BSTR (the empty string)
        return write_oahwnd(m.arg_ptr(1), 0);
    case 2:
        return put_i32(st->video_window_style);
    case 3:
        return write_i32(m.arg_ptr(1), st->video_window_style);
    case 4:
        return put_i32(st->video_window_style_ex);
    case 5:
        return write_i32(m.arg_ptr(1), st->video_window_style_ex);
    case 6:
        return put_i32(st->video_auto_show);
    case 7:
        return write_i32(m.arg_ptr(1), st->video_auto_show);
    case 8:
        return put_i32(st->video_window_state);
    case 9:
        return write_i32(m.arg_ptr(1), st->video_window_state);
    case 10:
        return put_i32(st->video_background_palette);
    case 11:
        return write_i32(m.arg_ptr(1), st->video_background_palette);
    case 12:
        return put_i32(st->video_visible);
    case 13:
        return write_i32(m.arg_ptr(1), st->video_visible);
    case 14:
        st->video_position_set = true;
        return put_i32(st->video_left);
    case 15:
        return write_i32(m.arg_ptr(1), st->video_left);
    case 16:
        st->video_position_set = true;
        return put_i32(st->video_width);
    case 17:
        return write_i32(m.arg_ptr(1), st->video_width);
    case 18:
        st->video_position_set = true;
        return put_i32(st->video_top);
    case 19:
        return write_i32(m.arg_ptr(1), st->video_top);
    case 20:
        st->video_position_set = true;
        return put_i32(st->video_height);
    case 21:
        return write_i32(m.arg_ptr(1), st->video_height);
    case 22: { // put_Owner
        st->video_owner = arg_oahwnd(1);
        if (st->video_owner == 0) {
            st->video_window_style &= static_cast<i32>(~WS_CHILD);
        } else {
            st->video_window_style |= static_cast<i32>(WS_CHILD);
        }
        graph_diag("put_owner sid=%u hwnd=0x%llx\n",
                   find_filter_graph_id_by_iface(m.arg_ptr(0)),
                   static_cast<unsigned long long>(st->video_owner));
        return com::S_OK;
    }
    case 23: // get_Owner(OAHWND*)
        return write_oahwnd(m.arg_ptr(1), st->video_owner);
    case 24: // put_MessageDrain(OAHWND)
        st->video_message_drain = arg_oahwnd(1);
        return com::S_OK;
    case 25: // get_MessageDrain(OAHWND*)
        return write_oahwnd(m.arg_ptr(1), st->video_message_drain);
    case 26:
        return write_i32(m.arg_ptr(1), st->video_border_color);
    case 27:
        return put_i32(st->video_border_color);
    case 28:
        return write_i32(m.arg_ptr(1), st->video_fullscreen);
    case 29:
        return put_i32(st->video_fullscreen);
    case 30: // SetWindowForeground
        return com::S_OK;
    case 31: // NotifyOwnerMessage
        return com::S_OK;
    case 32: // SetWindowPosition
        st->video_left = static_cast<i32>(m.arg32(1));
        st->video_top = static_cast<i32>(m.arg32(2));
        st->video_width = static_cast<i32>(m.arg32(3));
        st->video_height = static_cast<i32>(m.arg32(4));
        st->video_position_set = true;
        if (st->filter_state != 0) {
            present_filter_graph_video(*st, &m);
        }
        return com::S_OK;
    case 33: { // GetWindowPosition
        const u32 left_hr = write_i32(m.arg_ptr(1), st->video_left);
        if (left_hr != com::S_OK) return left_hr;
        const u32 top_hr = write_i32(m.arg_ptr(2), st->video_top);
        if (top_hr != com::S_OK) return top_hr;
        const u32 width_hr = write_i32(m.arg_ptr(3), st->video_width);
        if (width_hr != com::S_OK) return width_hr;
        return write_i32(m.arg_ptr(4), st->video_height);
    }
    case 34: { // GetMinIdealImageSize
        const u32 width_hr = write_i32(m.arg_ptr(1), st->video_width);
        if (width_hr != com::S_OK) return width_hr;
        return write_i32(m.arg_ptr(2), st->video_height);
    }
    case 35: { // GetMaxIdealImageSize
        const u32 width_hr = write_i32(m.arg_ptr(1), st->video_width);
        if (width_hr != com::S_OK) return width_hr;
        return write_i32(m.arg_ptr(2), st->video_height);
    }
    case 36: { // GetRestorePosition
        const u32 left_hr = write_i32(m.arg_ptr(1), st->video_left);
        if (left_hr != com::S_OK) return left_hr;
        const u32 top_hr = write_i32(m.arg_ptr(2), st->video_top);
        if (top_hr != com::S_OK) return top_hr;
        const u32 width_hr = write_i32(m.arg_ptr(3), st->video_width);
        if (width_hr != com::S_OK) return width_hr;
        return write_i32(m.arg_ptr(4), st->video_height);
    }
    case 37:
        return put_i32(st->video_hide_cursor);
    case 38:
        return write_i32(m.arg_ptr(1), st->video_hide_cursor);
    default:
        return E_NOTIMPL;
    }
}

u32 ComCoreManager::filter_graph_basic_video(Marshaler& m, u32 op) {
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) return com::E_INVALIDARG;
    // Learn IBasicVideo: Filter Graph Manager exposes the interface before
    // the graph is built; methods return E_NOINTERFACE until a video renderer
    // is present.
    if (!st->video_renderer_connected) return E_NOINTERFACE;
    std::fprintf(stderr, "[ole32] IBasicVideo op=%u\n", op);

    const auto write_i32 = [&](GuestAddr ptr, i32 value) -> u32 {
        if (ptr == 0) return E_POINTER;
        (void)m.write_u32(ptr, static_cast<u32>(value));
        return com::S_OK;
    };
    const auto put_i32 = [&](i32& field, bool& using_default) -> u32 {
        field = static_cast<i32>(m.arg32(1));
        using_default = false;
        return com::S_OK;
    };
    const auto write_reftime = [&](GuestAddr ptr, double value) -> u32 {
        if (ptr == 0) return E_POINTER;
        u64 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        (void)m.write_u64(ptr, bits);
        return com::S_OK;
    };
    const auto write_rect = [&](i32 left, i32 top, i32 width,
                                i32 height) -> u32 {
        const u32 left_hr = write_i32(m.arg_ptr(1), left);
        if (left_hr != com::S_OK) return left_hr;
        const u32 top_hr = write_i32(m.arg_ptr(2), top);
        if (top_hr != com::S_OK) return top_hr;
        const u32 width_hr = write_i32(m.arg_ptr(3), width);
        if (width_hr != com::S_OK) return width_hr;
        return write_i32(m.arg_ptr(4), height);
    };

    switch (op) {
    case 0: // get_AvgTimePerFrame
        return write_reftime(m.arg_ptr(1), 1.0 / 30.0);
    case 1: // get_BitRate
        return write_i32(m.arg_ptr(1), 0);
    case 2: // get_BitErrorRate
        return write_i32(m.arg_ptr(1), 0);
    case 3: // get_VideoWidth
        return write_i32(m.arg_ptr(1), st->native_video_width);
    case 4: // get_VideoHeight
        return write_i32(m.arg_ptr(1), st->native_video_height);
    case 5:
        return put_i32(st->source_left, st->default_source);
    case 6:
        return write_i32(m.arg_ptr(1), st->source_left);
    case 7:
        return put_i32(st->source_width, st->default_source);
    case 8:
        return write_i32(m.arg_ptr(1), st->source_width);
    case 9:
        return put_i32(st->source_top, st->default_source);
    case 10:
        return write_i32(m.arg_ptr(1), st->source_top);
    case 11:
        return put_i32(st->source_height, st->default_source);
    case 12:
        return write_i32(m.arg_ptr(1), st->source_height);
    case 13:
        return put_i32(st->dest_left, st->default_dest);
    case 14:
        return write_i32(m.arg_ptr(1), st->dest_left);
    case 15:
        return put_i32(st->dest_width, st->default_dest);
    case 16:
        return write_i32(m.arg_ptr(1), st->dest_width);
    case 17:
        return put_i32(st->dest_top, st->default_dest);
    case 18:
        return write_i32(m.arg_ptr(1), st->dest_top);
    case 19:
        return put_i32(st->dest_height, st->default_dest);
    case 20:
        return write_i32(m.arg_ptr(1), st->dest_height);
    case 21: // SetSourcePosition
        st->source_left = static_cast<i32>(m.arg32(1));
        st->source_top = static_cast<i32>(m.arg32(2));
        st->source_width = static_cast<i32>(m.arg32(3));
        st->source_height = static_cast<i32>(m.arg32(4));
        st->default_source = false;
        return com::S_OK;
    case 22: // GetSourcePosition
        return write_rect(st->source_left, st->source_top, st->source_width,
                          st->source_height);
    case 23: // SetDefaultSourcePosition
        st->source_left = 0;
        st->source_top = 0;
        st->source_width = st->native_video_width;
        st->source_height = st->native_video_height;
        st->default_source = true;
        return com::S_OK;
    case 24: // SetDestinationPosition
        st->dest_left = static_cast<i32>(m.arg32(1));
        st->dest_top = static_cast<i32>(m.arg32(2));
        st->dest_width = static_cast<i32>(m.arg32(3));
        st->dest_height = static_cast<i32>(m.arg32(4));
        st->default_dest = false;
        return com::S_OK;
    case 25: // GetDestinationPosition
        return write_rect(st->dest_left, st->dest_top, st->dest_width,
                          st->dest_height);
    case 26: // SetDefaultDestinationPosition
        st->dest_left = 0;
        st->dest_top = 0;
        st->dest_width = st->video_width != 0 ? st->video_width
                                              : st->native_video_width;
        st->dest_height = st->video_height != 0 ? st->video_height
                                                : st->native_video_height;
        st->default_dest = true;
        return com::S_OK;
    case 27: { // GetVideoSize
        const u32 width_hr = write_i32(m.arg_ptr(1), st->native_video_width);
        if (width_hr != com::S_OK) return width_hr;
        return write_i32(m.arg_ptr(2), st->native_video_height);
    }
    case 28: // GetVideoPaletteEntries
        if (m.arg_ptr(3) != 0) {
            (void)m.write_u32(m.arg_ptr(3), 0);
        }
        return com::S_OK;
    case 29: { // GetCurrentImage
        // Learn IBasicVideo::GetCurrentImage: pBufferSize in/out; NULL
        // pDIBImage returns the required DIB size. The buffer is a
        // BITMAPINFOHEADER plus pixels. VMR services this in any graph state.
        const GuestAddr size_ptr = m.arg_ptr(1);
        const GuestAddr dib_ptr = m.arg_ptr(2);
        if (size_ptr == 0) return E_POINTER;
        std::vector<u8> rgba;
        u32 width = 0;
        u32 height = 0;
        u64 frame_index = 0;
        if (st->player == nullptr ||
            !st->player->copy_video_frame(rgba, width, height, frame_index) ||
            width == 0 || height == 0) {
            std::fprintf(stderr,
                         "[ole32] IBasicVideo::GetCurrentImage E_FAIL\n");
            return E_FAIL;
        }
        const u32 header_bytes = 40;
        const u32 image_bytes = width * height * 4u;
        const u32 need = header_bytes + image_bytes;
        if (dib_ptr == 0) {
            (void)m.write_u32(size_ptr, need);
            std::fprintf(stderr,
                         "[ole32] IBasicVideo::GetCurrentImage size=%u "
                         "%ux%u frame=%llu\n",
                         need, width, height,
                         static_cast<unsigned long long>(frame_index));
            return com::S_OK;
        }
        const u32 have = m.memory().read_u32(size_ptr).value_or(0);
        if (have < need) {
            (void)m.write_u32(size_ptr, need);
            return 0x8007000Eu; // E_OUTOFMEMORY
        }
        std::vector<u8> dib(need, 0);
        auto write_u32_at = [&](u32 offset, u32 value) {
            std::memcpy(dib.data() + offset, &value, sizeof(value));
        };
        write_u32_at(0, header_bytes);
        write_u32_at(4, width);
        write_u32_at(8, height);
        dib[12] = 1;
        dib[13] = 0;
        dib[14] = 32;
        dib[15] = 0;
        write_u32_at(20, image_bytes);
        for (u32 y = 0; y < height; ++y) {
            const u8* src = rgba.data() + static_cast<usize>(y) * width * 4u;
            u8* dst = dib.data() + header_bytes +
                      static_cast<usize>(height - 1u - y) * width * 4u;
            for (u32 x = 0; x < width; ++x) {
                dst[x * 4u + 0] = src[x * 4u + 2];
                dst[x * 4u + 1] = src[x * 4u + 1];
                dst[x * 4u + 2] = src[x * 4u + 0];
                dst[x * 4u + 3] = 0;
            }
        }
        (void)m.memory().write_bytes(
            dib_ptr, reinterpret_cast<const Byte*>(dib.data()), dib.size());
        (void)m.write_u32(size_ptr, need);
        std::fprintf(stderr,
                     "[ole32] IBasicVideo::GetCurrentImage S_OK %ux%u "
                     "frame=%llu\n",
                     width, height,
                     static_cast<unsigned long long>(frame_index));
        return com::S_OK;
    }
    case 30: // IsUsingDefaultSource
        return st->default_source ? com::S_OK : com::S_FALSE;
    case 31: // IsUsingDefaultDestination
        return st->default_dest ? com::S_OK : com::S_FALSE;
    default:
        return E_NOTIMPL;
    }
}

} // namespace vitreo::winapi
