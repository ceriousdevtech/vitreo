// SPDX-License-Identifier: LGPL-2.1-only
/// @file filter_graph_guest_renderer.cpp
/// @brief Guest IBaseFilter renderer plumbing (IFilterGraph::AddFilter family):
/// video media-type negotiation, sample delivery and filter release.

#include "filter_graph_internal.hpp"
#include "winapi/com/ole32/dshow_host.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace vitreo::winapi {

namespace {

void write_guid_bytes(Marshaler& m, GuestAddr addr, const Guid& guid) {
    (void)m.write_struct<Guid>(addr, guid);
}

struct VideoOffer {
    Guid subtype{};
    u32 fourcc{0};
    u16 bit_count{32};
    u32 image_bytes{0};
};

[[nodiscard]] u32 video_image_bytes(u32 width, u32 height, u32 fourcc,
                                    u16 bit_count) {
    if (fourcc == FOURCC_YV12) {
        return width * height + (width * height) / 2u;
    }
    return width * height * (static_cast<u32>(bit_count) / 8u);
}

/// AM_MEDIA_TYPE + VIDEOINFOHEADER at the guest's layout (strmif.h /
/// amvideo.h, measured: kAmMediaType32/64). pbFormat points at `format`,
/// which the graph owns; pUnk is NULL.
void write_video_media_type(Marshaler& m, GuestAddr mt, GuestAddr format,
                            u32 width, u32 height, const VideoOffer& offer) {
    const AmMediaTypeLayout& layout = am_media_type_layout(m.cpu_is_64bit());
    const u32 image = offer.image_bytes;
    for (u32 i = 0; i < layout.size; i += 4) {
        (void)m.write_u32(mt + i, 0);
    }
    write_guid_bytes(m, mt + 0, MEDIATYPE_VIDEO);
    write_guid_bytes(m, mt + 16, offer.subtype);
    (void)m.write_u32(mt + 32, 1); // bFixedSizeSamples
    (void)m.write_u32(mt + 36, 0); // bTemporalCompression
    (void)m.write_u32(mt + 40, image); // lSampleSize
    write_guid_bytes(m, mt + 44, FORMAT_VIDEOINFO);
    (void)m.write_ptr(mt + layout.punk, 0);
    (void)m.write_u32(mt + layout.cb_format, VIDEOINFOHEADER_BYTES);
    (void)m.write_ptr(mt + layout.pb_format, format);
    for (u32 i = 0; i < VIDEOINFOHEADER_BYTES; i += 4) {
        (void)m.write_u32(format + i, 0);
    }
    (void)m.write_u64(format + 40, 333667); // AvgTimePerFrame ~29.97 fps
    (void)m.write_u32(format + 48, BITMAPINFOHEADER_BYTES); // biSize
    (void)m.write_u32(format + 52, width);                  // biWidth
    (void)m.write_u32(format + 56, height);                 // biHeight
    (void)m.write_u16(format + 60, 1);                      // biPlanes
    (void)m.write_u16(format + 62, offer.bit_count);        // biBitCount
    (void)m.write_u32(format + 64, offer.fourcc);           // biCompression
    (void)m.write_u32(format + 68, image);                  // biSizeImage
}

/// LPCWSTR as UTF-16 code units, exactly (FindFilterByName compares them
/// case-sensitively, measured).
[[nodiscard]] std::u16string read_u16_string(Marshaler& m, GuestAddr ptr) {
    std::u16string text;
    if (ptr == 0) return text;
    for (GuestAddr at = ptr; text.size() < 0x8000u; at += 2) {
        auto unit = m.memory().read_u16(at);
        if (!unit.is_ok() || unit.value() == 0) break;
        text.push_back(static_cast<char16_t>(unit.value()));
    }
    return text;
}

[[nodiscard]] std::u16string decimal4(u32 value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%04u", value);
    std::u16string out;
    for (const char* c = buffer; *c != 0; ++c) out.push_back(static_cast<char16_t>(*c));
    return out;
}

[[nodiscard]] std::string narrow(const std::u16string& text) {
    std::string out;
    for (const char16_t c : text) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return out;
}

void copy_rgba_to_bottom_up_bgra(const std::vector<u8>& rgba, u32 width,
                                 u32 height, u8* dest) {
    for (u32 y = 0; y < height; ++y) {
        const u8* src = rgba.data() +
                        static_cast<usize>(height - 1u - y) * width * 4u;
        u8* row = dest + static_cast<usize>(y) * width * 4u;
        for (u32 x = 0; x < width; ++x) {
            row[x * 4u + 0] = src[x * 4u + 2];
            row[x * 4u + 1] = src[x * 4u + 1];
            row[x * 4u + 2] = src[x * 4u + 0];
            row[x * 4u + 3] = src[x * 4u + 3];
        }
    }
}

void copy_rgba_to_yv12(const std::vector<u8>& rgba, u32 width, u32 height,
                       u8* dest) {
    u8* y_plane = dest;
    u8* v_plane = dest + static_cast<usize>(width) * height;
    u8* u_plane = v_plane + static_cast<usize>(width / 2u) * (height / 2u);
    for (u32 y = 0; y < height; ++y) {
        const u8* src = rgba.data() + static_cast<usize>(y) * width * 4u;
        for (u32 x = 0; x < width; ++x) {
            const int r = src[x * 4u + 0];
            const int g = src[x * 4u + 1];
            const int b = src[x * 4u + 2];
            const int yy =
                ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            y_plane[y * width + x] =
                static_cast<u8>(std::clamp(yy, 0, 255));
            if ((y & 1u) == 0 && (x & 1u) == 0) {
                const int uu =
                    ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                const int vv =
                    ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                const u32 cx = x / 2u;
                const u32 cy = y / 2u;
                const u32 chroma = cy * (width / 2u) + cx;
                u_plane[chroma] = static_cast<u8>(std::clamp(uu, 0, 255));
                v_plane[chroma] = static_cast<u8>(std::clamp(vv, 0, 255));
            }
        }
    }
}

} // namespace

u32 ComCoreManager::guest_vtable_call(Marshaler& m, GuestAddr punk, u32 slot,
                                      const u64* args, usize count) {
    if (punk == 0) return E_FAIL;
    // Host-implemented DirectShow objects (File Source, DirectSound
    // Renderer, allocators, clocks) answer directly; args[0] is `this`.
    dshow::Iface kind{};
    if (m_dshow != nullptr && m_dshow->find(punk, &kind) != nullptr) {
        const std::vector<u64> logical = dshow::from_guest(
            m.cpu_is_64bit(), kind, slot,
            std::span<const u64>(args + (count > 0 ? 1 : 0), count > 0 ? count - 1 : 0));
        return m_dshow->call(m, punk, kind, slot, logical);
    }
    if (!m_guest_call) return E_FAIL;
    auto vtable_r = m.read_ptr(punk);
    if (!vtable_r.is_ok() || vtable_r.value() == 0) return E_FAIL;
    const u32 ptr_size = m.cpu_is_64bit() ? 8u : 4u;
    auto method_r = m.read_ptr(static_cast<GuestAddr>(vtable_r.value()) +
                              static_cast<GuestAddr>(slot) * ptr_size);
    if (!method_r.is_ok() || method_r.value() == 0) return E_FAIL;
    return static_cast<u32>(m_guest_call(
        static_cast<GuestAddr>(method_r.value()), args, count,
        m.cpu_is_64bit()));
}

void ComCoreManager::connect_guest_video_renderer(Marshaler& m,
                                                  FilterGraphState& st) {
    if (st.guest_video_connected) return;
    if (st.player == nullptr || !st.player->has_video()) return;
    if (st.guest_filters.empty() || !m_guest_call) return;
    u32 width = 0;
    u32 height = 0;
    st.player->native_video_size(width, height);
    if (width == 0 || height == 0) return;

    const GuestAddr output = ensure_graph_output_pin(m, st);
    if (output == 0) return;
    if (st.media_type == 0) {
        const GuestAddr block = reserve_com_scratch(
            AM_MEDIA_TYPE_MAX_BYTES + VIDEOINFOHEADER_BYTES + 16u);
        if (block == 0) return;
        st.media_type = (block + 15u) & ~GuestAddr{15u};
        st.format_block = st.media_type + AM_MEDIA_TYPE_MAX_BYTES;
    }
    const VideoOffer offers[] = {
        {MEDIASUBTYPE_YV12, FOURCC_YV12, 12,
         video_image_bytes(width, height, FOURCC_YV12, 12)},
        {MEDIASUBTYPE_RGB32, 0, 32,
         video_image_bytes(width, height, 0, 32)},
        {MEDIASUBTYPE_RGB24, 0, 24,
         video_image_bytes(width, height, 0, 24)},
    };

    // Out-parameters for the calls below: IEnumPins* / IPin* / void* are
    // pointer-width (8-byte slots, 8-aligned); ULONG and PIN_DIRECTION are
    // 4 bytes in both bitnesses.
    const GuestAddr scratch = co_task_mem_alloc_guest(96);
    if (scratch == 0) return;
    struct ScratchGuard {
        ComCoreManager* mgr;
        GuestAddr addr;
        ~ScratchGuard() { mgr->co_task_mem_free_guest(addr); }
    } guard{this, scratch};
    const GuestAddr enum_ptr = scratch;
    const GuestAddr pin_ptr = scratch + 8;
    const GuestAddr fetched_ptr = scratch + 16;
    const GuestAddr dir_ptr = scratch + 20;
    const GuestAddr qi_iid = scratch + 24;
    const GuestAddr qi_out = scratch + 40;
    const GuestAddr props_wanted = scratch + 48;  // ALLOCATOR_PROPERTIES (16)
    const GuestAddr props_actual = scratch + 64;

    for (auto& filter : st.guest_filters) {
        (void)m.write_ptr(enum_ptr, 0);
        // IBaseFilter::EnumPins (slot 10).
        const u64 enum_args[] = {filter.filter, enum_ptr};
        if (guest_vtable_call(m, filter.filter, 10, enum_args, 2) !=
            com::S_OK) {
            continue;
        }
        auto enum_r = m.read_ptr(enum_ptr);
        if (!enum_r.is_ok() || enum_r.value() == 0) continue;
        const GuestAddr enumerator = static_cast<GuestAddr>(enum_r.value());
        GuestAddr input = 0;
        for (u32 n = 0; n < 8 && input == 0; ++n) {
            (void)m.write_ptr(pin_ptr, 0);
            (void)m.write_u32(fetched_ptr, 0);
            // IEnumPins::Next (slot 3).
            const u64 next_args[] = {enumerator, 1, pin_ptr, fetched_ptr};
            if (guest_vtable_call(m, enumerator, 3, next_args, 4) !=
                com::S_OK) {
                break;
            }
            auto pin_r = m.read_ptr(pin_ptr);
            if (!pin_r.is_ok() || pin_r.value() == 0) break;
            const GuestAddr pin = static_cast<GuestAddr>(pin_r.value());
            (void)m.write_u32(dir_ptr, 0xFFFFFFFFu);
            // IPin::QueryDirection (slot 9).
            const u64 dir_args[] = {pin, dir_ptr};
            const u32 dir_hr = guest_vtable_call(m, pin, 9, dir_args, 2);
            auto dir_r = m.read_u32(dir_ptr);
            if (dir_hr == com::S_OK && dir_r.is_ok() &&
                dir_r.value() == PINDIR_INPUT) {
                input = pin;
            } else {
                const u64 rel[] = {pin};
                (void)guest_vtable_call(m, pin, 2, rel, 1);
            }
        }
        const u64 rel_enum[] = {enumerator};
        (void)guest_vtable_call(m, enumerator, 2, rel_enum, 1);
        if (input == 0) continue;

        u32 hr = VFW_E_TYPE_NOT_ACCEPTED;
        const VideoOffer* accepted = nullptr;
        for (const auto& offer : offers) {
            write_video_media_type(m, st.media_type, st.format_block, width,
                                   height, offer);
            // IPin::QueryAccept (slot 11).
            const u64 accept_args[] = {input, st.media_type};
            const u32 accept_hr =
                guest_vtable_call(m, input, 11, accept_args, 2);
            if (accept_hr == VFW_E_TYPE_NOT_ACCEPTED ||
                accept_hr == com::S_FALSE) {
                continue;
            }
            // IPin::ReceiveConnection (slot 4).
            const u64 connect_args[] = {input, output, st.media_type};
            hr = guest_vtable_call(m, input, 4, connect_args, 3);
            std::fprintf(stderr,
                         "[ole32] RenderFile connect guest renderer "
                         "filter=0x%llx pin=0x%llx fourcc=0x%x -> 0x%x %ux%u\n",
                         static_cast<unsigned long long>(filter.filter),
                         static_cast<unsigned long long>(input), offer.fourcc,
                         hr, width, height);
            if (hr == com::S_OK) {
                accepted = &offer;
                break;
            }
        }
        if (accepted == nullptr) {
            const u64 rel[] = {input};
            (void)guest_vtable_call(m, input, 2, rel, 1);
            continue;
        }
        st.guest_video_fourcc = accepted->fourcc;
        filter.input_pin = input;
        write_guid_bytes(m, qi_iid, IID_IMEMINPUTPIN);
        (void)m.write_ptr(qi_out, 0);
        (void)qi_iid;
        filter.mem_input = dshow_host().query(m, input, IID_IMEMINPUTPIN);
        const u32 image_bytes = accepted->image_bytes;
        if (filter.mem_input != 0) {
            // CBaseOutputPin::DecideAllocator: the input pin's requirements
            // (IMemInputPin::GetAllocatorRequirements, slot 5; E_NOTIMPL is
            // common), its allocator (GetAllocator, slot 3), the buffer size
            // (IMemAllocator::SetProperties, slot 3), then NotifyAllocator
            // (slot 4). ALLOCATOR_PROPERTIES is four longs in both bitnesses.
            for (u32 i = 0; i < 32; i += 4) (void)m.write_u32(props_wanted + i, 0);
            const u64 req_args[] = {filter.mem_input, props_wanted};
            (void)guest_vtable_call(m, filter.mem_input, 5, req_args, 2);
            const u32 req_buffers = m.read_u32(props_wanted + 0).value_or(0);
            const u32 req_bytes = m.read_u32(props_wanted + 4).value_or(0);
            const u32 req_align = m.read_u32(props_wanted + 8).value_or(0);
            const u32 req_prefix = m.read_u32(props_wanted + 12).value_or(0);
            (void)m.write_u32(props_wanted + 0, std::max<u32>(req_buffers, 1));
            (void)m.write_u32(props_wanted + 4, std::max(req_bytes, image_bytes));
            (void)m.write_u32(props_wanted + 8, std::max<u32>(req_align, 1));
            (void)m.write_u32(props_wanted + 12, req_prefix);
            (void)m.write_ptr(qi_out, 0);
            const u64 alloc_args[] = {filter.mem_input, qi_out};
            if (guest_vtable_call(m, filter.mem_input, 3, alloc_args, 2) ==
                com::S_OK) {
                auto alloc = m.read_ptr(qi_out);
                if (alloc.is_ok()) filter.allocator = static_cast<GuestAddr>(alloc.value());
            }
            if (filter.allocator != 0) {
                const u64 set_args[] = {filter.allocator, props_wanted, props_actual};
                const u32 set_hr = guest_vtable_call(m, filter.allocator, 3, set_args, 3);
                const u32 actual_bytes = m.read_u32(props_actual + 4).value_or(0);
                if (static_cast<i32>(set_hr) < 0 || actual_bytes < image_bytes) {
                    std::fprintf(stderr,
                                 "[ole32] guest allocator SetProperties -> 0x%x "
                                 "cbBuffer=%u (need %u): graph sample used\n",
                                 set_hr, actual_bytes, image_bytes);
                    const u64 rel[] = {filter.allocator};
                    (void)guest_vtable_call(m, filter.allocator, 2, rel, 1);
                    filter.allocator = 0;
                }
            }
            if (filter.allocator != 0) {
                // NotifyAllocator(pAllocator, bReadOnly = FALSE).
                const u64 notify_args[] = {filter.mem_input, filter.allocator,
                                           0};
                (void)guest_vtable_call(m, filter.mem_input, 4, notify_args,
                                        3);
            }
            // The streaming thread's state. Without a usable allocator the
            // graph's own IMediaSample carries the frames.
            auto stream = std::make_shared<FilterGraphStream>();
            stream->filter_graph_id = find_filter_graph_id_by_iface(st.graph_builder_iface);
            stream->player = st.player;
            stream->mem_input = filter.mem_input;
            stream->input_pin = filter.input_pin;
            stream->allocator = filter.allocator;
            stream->fourcc = accepted->fourcc;
            stream->width = width;
            stream->height = height;
            stream->scratch = co_task_mem_alloc_guest(32);
            if (filter.allocator == 0) {
                stream->own_sample = ensure_graph_media_sample(m, st, image_bytes);
                stream->own_buffer = st.sample_buffer;
                stream->own_buffer_bytes = st.sample_buffer_bytes;
            }
            if (stream->scratch != 0 &&
                (stream->allocator != 0 || stream->own_sample != 0)) {
                std::lock_guard<std::mutex> lock(m_filter_graph_clock_mutex);
                st.stream = std::move(stream);
            } else if (stream->scratch != 0) {
                co_task_mem_free_guest(stream->scratch);
            }
        }
        st.guest_video_connected = true;
        st.video_renderer_connected = true;
        return;
    }
}

// ---------------------------------------------------------------------------
// State transitions and the streaming thread.
//
// The graph's source side (Vitreo's decoder) behaves as a CSourceStream
// output pin: it delivers on a guest thread of its own, never on the thread
// that called IMediaControl. A renderer's IMemInputPin::Receive blocks while
// the renderer is paused with a sample (the preroll), so delivering on the
// caller's thread would hang Pause. Filter Graph Manager behaviour measured
// on Windows 11 x86 and x64 (tools/oracle_probe/dshow_state_probe):
//   Pause:    each filter's Pause; if any returned S_FALSE, GetState(0) on
//             the filters and S_FALSE while one is VFW_S_STATE_INTERMEDIATE.
//             A failing Pause stops the filters and is the result.
//   Paused -> Pause: GetState(0) only.
//   GetState(ms): polls the filters (timeouts 0, 10, 0, 10, ...) until none
//             is intermediate or `ms` has passed; *pfs is the target state;
//             VFW_S_STATE_INTERMEDIATE / VFW_S_CANT_CUE are passed through.
//   Run from Stopped: Pause first; while cueing, Run returns S_FALSE and the
//             filters' Run arrives later from another thread.
//   Run from Paused: GetState(0), then Run.
//   Stop from Running: Pause, then Stop; a filter's S_FALSE is the result.
//   Final Release: Stop.
// ---------------------------------------------------------------------------

std::vector<GuestAddr> ComCoreManager::guest_filter_list(const FilterGraphState& st) const {
    // Most recently added first, the order EnumFilters reports.
    std::vector<GuestAddr> filters;
    for (auto it = st.guest_filters.rbegin(); it != st.guest_filters.rend(); ++it) {
        if (it->filter != 0) filters.push_back(it->filter);
    }
    return filters;
}

u32 ComCoreManager::call_guest_filters_state(Marshaler& m,
                                             const std::vector<GuestAddr>& filters,
                                             u32 slot, i64 start) {
    u32 result = com::S_OK;
    for (const GuestAddr filter : filters) {
        u32 hr = 0;
        if (slot == 6) {
            // IMediaFilter::Run(REFERENCE_TIME tStart): one 8-byte argument
            // on x64, two stack dwords (low, high) on x86. Without a graph
            // clock (host-decode graphs) the start time is 0 and renderers
            // present on arrival.
            const u64 t = static_cast<u64>(start);
            const u64 args[] = {filter, m.cpu_is_64bit() ? t : (t & 0xFFFFFFFFull),
                                t >> 32};
            hr = guest_vtable_call(m, filter, 6, args, m.cpu_is_64bit() ? 2 : 3);
        } else {
            const u64 args[] = {filter};
            hr = guest_vtable_call(m, filter, slot, args, 1);
        }
        static constexpr const char* kVerbs[] = {"", "", "", "", "Stop", "Pause", "Run"};
        std::fprintf(stderr, "[quartz] %s filter 0x%llx -> 0x%x\n",
                     slot < 7 ? kVerbs[slot] : "?", static_cast<unsigned long long>(filter), hr);
        if (static_cast<i32>(hr) < 0) {
            if (static_cast<i32>(result) >= 0) result = hr;
        } else if (hr == com::S_FALSE && result == com::S_OK) {
            result = com::S_FALSE;
        }
    }
    return result;
}

u32 ComCoreManager::query_guest_filters_state(Marshaler& m,
                                              const std::vector<GuestAddr>& filters,
                                              u32 timeout_ms) {
    if (filters.empty()) return com::S_OK;
    const GuestAddr state = co_task_mem_alloc_guest(8);
    if (state == 0) return E_OUTOFMEMORY;
    u32 result = com::S_OK;
    for (const GuestAddr filter : filters) {
        // IMediaFilter::GetState(DWORD dwMilliSecsTimeout, FILTER_STATE*)
        (void)m.write_u32(state, 0);
        const u64 args[] = {filter, timeout_ms, state};
        const u32 hr = guest_vtable_call(m, filter, 7, args, 3);
        if (static_cast<i32>(hr) < 0) {
            result = hr;
            break;
        }
        if (hr == VFW_S_STATE_INTERMEDIATE) {
            result = VFW_S_STATE_INTERMEDIATE;
        } else if (hr == VFW_S_CANT_CUE && result == com::S_OK) {
            result = VFW_S_CANT_CUE;
        }
    }
    co_task_mem_free_guest(state);
    return result;
}

u32 ComCoreManager::count_guest_renderers(Marshaler& m,
                                          const std::vector<GuestAddr>& filters) {
    const GuestAddr scratch = co_task_mem_alloc_guest(32);
    if (scratch == 0) return 0;
    write_guid_bytes(m, scratch, IID_IAMFILTERMISCFLAGS);
    u32 count = 0;
    for (const GuestAddr filter : filters) {
        (void)m.write_ptr(scratch + 16, 0);
        const GuestAddr flags = dshow_host().query(m, filter, IID_IAMFILTERMISCFLAGS);
        if (flags == 0) continue;
        const u64 self[] = {flags};
        const u32 value = guest_vtable_call(m, flags, 3, self, 1);  // GetMiscFlags
        if ((value & AM_FILTER_MISC_FLAGS_IS_RENDERER) != 0) ++count;
        (void)guest_vtable_call(m, flags, 2, self, 1);
    }
    co_task_mem_free_guest(scratch);
    return count;
}

bool ComCoreManager::start_guest_stream(Marshaler& m, FilterGraphState& st, u32 target) {
    const std::shared_ptr<FilterGraphStream> s = st.stream;
    if (s == nullptr) return false;
    bool launch = false;
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        s->filters = guest_filter_list(st);
        if (!s->thread_alive) {
            s->exit = false;
            s->prerolled = false;
            s->halted = false;
            s->eos_sent = false;
            s->discontinuity = true;
            s->last_frame = ~u64{0};
            s->segment_start = s->player != nullptr ? s->player->position_100ns() : 0;
            s->thread_alive = true;
            launch = true;
        }
        // CSourceStream: after Receive S_FALSE the thread stops sending
        // until its next Pause/Run command.
        s->halted = false;
        s->target = target;
        ++s->generation;
    }
    s->cv.notify_all();
    if (!launch) return true;
    {
        std::lock_guard<std::mutex> lock(m_filter_graph_map_mutex);
        m_filter_streams[s->filter_graph_id] = s;
    }
    bool started = false;
    if (m_thunks != nullptr && m_audio_activation_thread) {
        auto entry = m_thunks->get_thunk_address("ole32.dll", "Vitreo_FilterGraph_StreamThread", true);
        started = entry.is_ok() &&
                  m_audio_activation_thread(entry.value(), s->filter_graph_id);
    }
    (void)m;
    if (started) return true;
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        s->thread_alive = false;
    }
    {
        std::lock_guard<std::mutex> lock(m_filter_graph_map_mutex);
        m_filter_streams.erase(s->filter_graph_id);
    }
    VITREO_QUARTZ_UNIMPLEMENTED_ONCE(
        "IMediaControl: no guest streaming thread could be started");
    return false;
}

void ComCoreManager::stop_guest_stream(FilterGraphState& st) {
    const std::shared_ptr<FilterGraphStream> s = st.stream;
    if (s == nullptr) return;
    std::unique_lock<std::mutex> lock(s->mutex);
    s->exit = true;
    s->target = 0;
    s->run_pending = false;
    ++s->generation;
    ++s->flushes;
    s->cv.notify_all();
    // CSourceStream::Stop waits for its thread without a timeout; so does
    // this. The renderers' Stop has already released a blocked Receive and
    // the allocator's Decommit a blocked GetBuffer.
    auto waited = std::chrono::steady_clock::now();
    while (s->thread_alive || s->run_worker_alive) {
        s->cv.wait_for(lock, std::chrono::milliseconds(500));
        if (std::chrono::steady_clock::now() - waited > std::chrono::seconds(5)) {
            std::fprintf(stderr,
                         "[ole32] filter graph %u: still waiting for the streaming "
                         "thread to leave the renderer\n",
                         s->filter_graph_id);
            waited = std::chrono::steady_clock::now();
        }
    }
    lock.unlock();
    std::lock_guard<std::mutex> map_lock(m_filter_graph_map_mutex);
    auto it = m_filter_streams.find(s->filter_graph_id);
    if (it != m_filter_streams.end() && it->second == s) m_filter_streams.erase(it);
}

u32 ComCoreManager::deliver_stream_sample(Marshaler& m, FilterGraphStream& s,
                                          const std::vector<u8>& rgba, u32 width,
                                          u32 height, i64 start_100ns) {
    const u32 bytes = video_image_bytes(width, height, s.fourcc,
                                        s.fourcc == FOURCC_YV12 ? 12 : 32);
    if (bytes == 0 || rgba.size() < static_cast<usize>(width) * height * 4u) return E_FAIL;
    std::vector<u8> converted(bytes);
    if (s.fourcc == FOURCC_YV12) {
        copy_rgba_to_yv12(rgba, width, height, converted.data());
    } else {
        copy_rgba_to_bottom_up_bgra(rgba, width, height, converted.data());
    }
    const GuestAddr out = s.scratch;        // IMediaSample* / BYTE*
    const GuestAddr time = s.scratch + 16;  // REFERENCE_TIME tStart
    (void)m.write_u64(time, static_cast<u64>(start_100ns));
    bool discontinuity = false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        discontinuity = s.discontinuity;
    }

    if (s.allocator != 0) {
        // IMemAllocator::GetBuffer(ppBuffer, NULL, NULL, 0) (slot 7): blocks
        // while every buffer is held; VFW_E_NOT_COMMITTED once decommitted.
        (void)m.write_ptr(out, 0);
        const u64 get_args[] = {s.allocator, out, 0, 0, 0};
        const u32 get_hr = guest_vtable_call(m, s.allocator, 7, get_args, 5);
        if (get_hr != com::S_OK) return get_hr;
        const GuestAddr sample = static_cast<GuestAddr>(m.read_ptr(out).value_or(0));
        if (sample == 0) return E_FAIL;
        (void)m.write_ptr(out + 8, 0);
        const u64 ptr_args[] = {sample, out + 8};  // GetPointer (slot 3)
        const u32 ptr_hr = guest_vtable_call(m, sample, 3, ptr_args, 2);
        const GuestAddr buffer = static_cast<GuestAddr>(m.read_ptr(out + 8).value_or(0));
        const u64 self[] = {sample};
        const u32 size = guest_vtable_call(m, sample, 4, self, 1);  // GetSize
        if (ptr_hr != com::S_OK || buffer == 0 || static_cast<i32>(size) < static_cast<i32>(bytes)) {
            (void)guest_vtable_call(m, sample, 2, self, 1);
            return E_FAIL;
        }
        (void)m.memory().write_bytes(buffer, converted.data(), bytes);
        const u64 len_args[] = {sample, bytes};
        (void)guest_vtable_call(m, sample, 12, len_args, 2);    // SetActualDataLength
        const u64 time_args[] = {sample, time, 0};
        (void)guest_vtable_call(m, sample, 6, time_args, 3);    // SetTime(&start, NULL)
        const u64 sync_args[] = {sample, 1};
        (void)guest_vtable_call(m, sample, 8, sync_args, 2);    // SetSyncPoint(TRUE)
        const u64 disc_args[] = {sample, discontinuity ? 1u : 0u};
        (void)guest_vtable_call(m, sample, 16, disc_args, 2);   // SetDiscontinuity
        const u64 recv[] = {s.mem_input, sample};
        const u32 hr = guest_vtable_call(m, s.mem_input, 6, recv, 2);  // Receive
        (void)guest_vtable_call(m, sample, 2, self, 1);
        return hr;
    }

    // The graph's own sample: wait until the renderer has let go of it (a
    // one-buffer allocator's GetBuffer would block the same way).
    FilterGraphState* st = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_filter_graph_map_mutex);
        auto it = m_filter_graphs.find(s.filter_graph_id);
        if (it != m_filter_graphs.end()) st = &it->second;
    }
    if (st == nullptr || s.own_sample == 0 || s.own_buffer_bytes < bytes) return E_FAIL;
    for (;;) {
        if (std::atomic_ref<u32>(st->sample_refs).load() <= 1) break;
        std::unique_lock<std::mutex> lock(s.mutex);
        if (s.exit || s.target == 0) return VFW_E_NOT_COMMITTED;
        s.cv.wait_for(lock, std::chrono::milliseconds(2));
    }
    (void)m.memory().write_bytes(s.own_buffer, converted.data(), bytes);
    st->sample_data_bytes = bytes;
    st->sample_start = start_100ns;
    st->sample_start_valid = true;
    st->sample_end_valid = false;
    st->sample_media_time_valid = false;
    st->sample_sync_point = true;
    st->sample_preroll = false;
    st->sample_discontinuity = discontinuity;
    const u64 recv[] = {s.mem_input, s.own_sample};
    return guest_vtable_call(m, s.mem_input, 6, recv, 2);
}

/// The graph's own audio rendering reached the end of the file: its
/// EC_COMPLETE, as the DirectSound Renderer would send it on Windows.
void ComCoreManager::report_host_audio_complete(u32 sid) {
    FilterGraphState* st = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_filter_graph_map_mutex);
        auto it = m_filter_graphs.find(sid);
        if (it != m_filter_graphs.end()) st = &it->second;
    }
    if (st == nullptr) return;
    std::lock_guard<std::mutex> lock(m_filter_graph_clock_mutex);
    if (!st->host_audio_renderer_pending) return;
    st->host_audio_renderer_pending = false;
    report_filter_graph_event_locked(*st, EC_COMPLETE, com::S_OK, 0);
}

u32 ComCoreManager::filter_graph_stream_thread(Marshaler& m) {
    const u32 sid = m.arg32(0);
    std::shared_ptr<FilterGraphStream> s;
    {
        std::lock_guard<std::mutex> lock(m_filter_graph_map_mutex);
        auto it = m_filter_streams.find(sid);
        if (it != m_filter_streams.end()) s = it->second;
    }
    if (s == nullptr) return 0;
    graph_diag("stream thread start sid=%u\n", sid);
    std::vector<u8> rgba;
    for (;;) {
        u32 target = 0;
        u32 generation = 0;
        u32 flushes = 0;
        {
            std::unique_lock<std::mutex> lock(s->mutex);
            // Idle until there is something to send: stopped, prerolled in
            // Paused, told to stop sending (Receive S_FALSE / failure), or
            // past the end — each lasts until the next state command.
            const u32 seen = s->generation;
            const auto idle = [&] {
                return s->target == 0 || s->halted || s->eos_sent ||
                       (s->target == 1 && s->prerolled);
            };
            while (!s->exit && idle()) {
                s->cv.wait(lock, [&] { return s->exit || s->generation != seen; });
                if (s->generation != seen) break;
            }
            if (s->exit) break;
            if (idle()) continue;
            target = s->target;
            generation = s->generation;
            flushes = s->flushes;
        }
        u32 width = 0;
        u32 height = 0;
        u64 frame_index = 0;
        const bool have = s->player != nullptr &&
                          s->player->copy_video_frame(rgba, width, height, frame_index);
        bool deliver = have;
        if (have && target == 2 && frame_index == s->last_frame) deliver = false;
        if (!deliver) {
            // Running past the last new frame: end of stream once the
            // decoder says playback is complete.
            if (target == 2 && s->player != nullptr && s->player->take_completed()) {
                const u64 eos[] = {s->input_pin};
                const u32 hr = guest_vtable_call(m, s->input_pin, 14, eos, 1);  // EndOfStream
                std::fprintf(stderr, "[ole32] stream sid=%u EndOfStream -> 0x%x after %u samples\n",
                             sid, hr, s->delivered);
                {
                    std::lock_guard<std::mutex> lock(s->mutex);
                    if (s->flushes == flushes) s->eos_sent = true;
                }
                report_host_audio_complete(sid);
                continue;
            }
            std::unique_lock<std::mutex> lock(s->mutex);
            s->cv.wait_for(lock, std::chrono::milliseconds(5),
                           [&] { return s->exit || s->generation != generation; });
            continue;
        }
        const i64 start = std::max<i64>(
            0, s->player->position_100ns() - s->segment_start);
        const u32 hr = deliver_stream_sample(m, *s, rgba, width, height, start);
        std::lock_guard<std::mutex> lock(s->mutex);
        if (s->flushes != flushes || s->exit) continue;  // flushed or stopped meanwhile
        s->last_frame = frame_index;
        if (hr == com::S_OK) {
            if (s->delivered == 0) {
                std::fprintf(stderr, "[ole32] stream sid=%u first sample %ux%u state=%u\n", sid,
                             width, height, target);
            }
            s->prerolled = true;
            s->discontinuity = false;
            ++s->delivered;
        } else {
            // IMemInputPin::Receive S_FALSE: stop sending until the next
            // state change or flush; a failure likewise.
            s->halted = true;
            std::fprintf(stderr, "[ole32] stream sid=%u Receive -> 0x%x after %u samples, halted\n",
                         sid, hr, s->delivered);
        }
        s->cv.notify_all();
    }
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        s->thread_alive = false;
    }
    s->cv.notify_all();
    std::fprintf(stderr, "[ole32] stream thread exit sid=%u after %u samples\n", sid,
                 s->delivered);
    return 0;
}

u32 ComCoreManager::filter_graph_run_worker(Marshaler& m) {
    const u32 sid = m.arg32(0);
    std::shared_ptr<FilterGraphStream> s;
    {
        std::lock_guard<std::mutex> lock(m_filter_graph_map_mutex);
        auto it = m_filter_streams.find(sid);
        if (it != m_filter_streams.end()) s = it->second;
    }
    if (s == nullptr) return 0;
    std::vector<GuestAddr> filters;
    u32 generation = 0;
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        filters = s->filters;
        generation = s->generation;
    }
    // Poll the renderers as the FGM does (0 / 10 ms timeouts) until the
    // cue completes, then run them.
    u32 step = 0;
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            if (s->exit || !s->run_pending || s->generation != generation) break;
        }
        const u32 hr = query_guest_filters_state(m, filters, step);
        step = step == 0 ? 10u : 0u;
        if (hr == VFW_S_STATE_INTERMEDIATE) continue;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            if (s->exit || !s->run_pending || s->generation != generation) break;
            s->run_pending = false;
        }
        if (s->player != nullptr) (void)s->player->run();
        (void)call_guest_filters_state(m, filters, 6, graph_run_start(m, s->clock));
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            if (!s->exit && s->generation == generation) {
                s->target = 2;
                ++s->generation;
            }
        }
        s->cv.notify_all();
        break;
    }
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        s->run_worker_alive = false;
    }
    s->cv.notify_all();
    return 0;
}

void ComCoreManager::release_guest_filters(Marshaler& m, FilterGraphState& st) {
    // Graph destruction / RemoveFilter: JoinFilterGraph(NULL, NULL) then
    // Release on each entry (measured: a filter added twice is joined and
    // released twice).
    for (auto& filter : st.guest_filters) {
        if (filter.filter != 0 && m_guest_call) {
            const u64 join[] = {filter.filter, 0, 0};
            (void)guest_vtable_call(m, filter.filter, 13, join, 3);
            if (filter.mem_input != 0 && filter.mem_input != filter.input_pin) {
                const u64 rel[] = {filter.mem_input};
                (void)guest_vtable_call(m, filter.mem_input, 2, rel, 1);
            }
            if (filter.allocator != 0) {
                const u64 rel[] = {filter.allocator};
                (void)guest_vtable_call(m, filter.allocator, 2, rel, 1);
            }
            if (filter.input_pin != 0) {
                const u64 rel[] = {filter.input_pin};
                (void)guest_vtable_call(m, filter.input_pin, 2, rel, 1);
            }
            const u64 rel[] = {filter.filter};
            (void)guest_vtable_call(m, filter.filter, 2, rel, 1);
        }
        if (filter.name_ptr != 0) {
            co_task_mem_free_guest(filter.name_ptr);
            filter.name_ptr = 0;
        }
    }
    if (!st.guest_filters.empty()) ++st.filter_list_version;
    st.guest_filters.clear();
    st.guest_video_connected = false;
}

void ComCoreManager::drop_guest_stream(FilterGraphState& st) {
    if (st.stream == nullptr) return;
    stop_guest_stream(st);
    if (st.stream->scratch != 0) co_task_mem_free_guest(st.stream->scratch);
    std::lock_guard<std::mutex> lock(m_filter_graph_clock_mutex);
    st.stream.reset();
}

u32 ComCoreManager::filter_graph_add_filter(Marshaler& m) {
    // HRESULT AddFilter(IBaseFilter* pFilter, LPCWSTR pName)
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) return com::E_INVALIDARG;
    const GuestAddr filter = m.arg_ptr(1);
    const GuestAddr name_arg = m.arg_ptr(2);
    if (filter == 0) return E_POINTER;
    const std::u16string requested = read_u16_string(m, name_arg);
    return add_filter_internal(m, *st, filter, name_arg != 0 ? &requested : nullptr);
}

u32 ComCoreManager::add_filter_internal(Marshaler& m, FilterGraphState& st_ref,
                                        GuestAddr filter, const std::u16string* requested_name) {
    FilterGraphState* st = &st_ref;
    // Names are unique within the graph. Measured (dshow_graph_names
    // probe, x86 and x64): a NULL or empty name becomes "%04u" of a graph
    // counter that starts at 1; a duplicate becomes "<name> %04u" and the
    // call returns VFW_S_DUPLICATE_NAME; a generated name that still
    // collides takes the next counter value. The counter advances only when
    // a name is generated. The same filter may be added more than once.
    const auto in_use = [&](const std::u16string& candidate) {
        for (const auto& existing : st->guest_filters) {
            if (existing.name == candidate) return true;
        }
        return false;
    };
    const std::u16string requested = requested_name != nullptr ? *requested_name : u"";
    std::u16string name = requested;
    u32 hr = com::S_OK;
    if (requested.empty()) {
        do {
            name = decimal4(st->filter_name_index++);
        } while (in_use(name));
    } else if (in_use(requested)) {
        do {
            name = requested + u" " + decimal4(st->filter_name_index++);
        } while (in_use(name));
        hr = VFW_S_DUPLICATE_NAME;
    }

    const GuestAddr name_ptr =
        co_task_mem_alloc_guest((static_cast<u64>(name.size()) + 1) * 2);
    if (name_ptr == 0) return E_OUTOFMEMORY;
    for (usize i = 0; i <= name.size(); ++i) {
        (void)m.write_u16(name_ptr + static_cast<GuestAddr>(i * 2),
                          i < name.size() ? static_cast<u16>(name[i]) : u16{0});
    }

    (void)dshow_host().add_ref(m, filter);
    // IBaseFilter::JoinFilterGraph(pGraph, pName) (slot 13). Measured: a
    // failure is AddFilter's result, the filter is released and not added.
    const u64 join[] = {filter, st->graph_builder_iface, name_ptr};
    const u32 join_hr = guest_vtable_call(m, filter, 13, join, 3);
    if (static_cast<i32>(join_hr) < 0) {
        (void)dshow_host().release(m, filter);
        co_task_mem_free_guest(name_ptr);
        std::fprintf(stderr,
                     "[ole32] IFilterGraph::AddFilter JoinFilterGraph "
                     "filter=0x%llx -> 0x%x\n",
                     static_cast<unsigned long long>(filter), join_hr);
        return join_hr;
    }
    FilterGraphState::GuestFilter entry{};
    entry.filter = filter;
    entry.name = name;
    entry.name_ptr = name_ptr;
    st->guest_filters.push_back(std::move(entry));
    ++st->filter_list_version;
    std::fprintf(stderr,
                 "[ole32] IFilterGraph::AddFilter filter=0x%llx name='%s' -> 0x%x\n",
                 static_cast<unsigned long long>(filter), narrow(name).c_str(),
                 hr);
    return hr;
}

u32 ComCoreManager::filter_graph_remove_filter(Marshaler& m) {
    // HRESULT RemoveFilter(IBaseFilter* pFilter)
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) return com::E_INVALIDARG;
    const GuestAddr filter = m.arg_ptr(1);
    if (filter == 0) return E_POINTER;
    // Most recently added entry first, the order EnumFilters reports.
    for (auto it = st->guest_filters.rbegin(); it != st->guest_filters.rend();
         ++it) {
        if (it->filter != filter) continue;
        FilterGraphState::GuestFilter one = *it;
        st->guest_filters.erase(std::next(it).base());
        if (one.input_pin != 0) {
            // The connected renderer leaves: its streaming thread goes first.
            drop_guest_stream(*st);
            st->guest_video_connected = false;
        }
        std::vector<FilterGraphState::GuestFilter> saved;
        saved.swap(st->guest_filters);
        st->guest_filters.push_back(one);
        const bool connected = st->guest_video_connected;
        release_guest_filters(m, *st);
        st->guest_video_connected = connected;
        st->guest_filters = std::move(saved);
        ++st->filter_list_version;
        return com::S_OK;
    }
    // Measured: a filter that is not in the graph still gets
    // JoinFilterGraph(NULL, NULL), is not released, and the call succeeds.
    const u64 join[] = {filter, 0, 0};
    (void)guest_vtable_call(m, filter, 13, join, 3);
    return com::S_OK;
}

u32 ComCoreManager::filter_graph_find_filter_by_name(Marshaler& m) {
    // HRESULT FindFilterByName(LPCWSTR pName, IBaseFilter** ppFilter)
    auto* st = find_filter_graph_by_iface(m.arg_ptr(0));
    if (st == nullptr) return com::E_INVALIDARG;
    const GuestAddr name_ptr = m.arg_ptr(1);
    const GuestAddr out = m.arg_ptr(2);
    // Measured: either NULL is E_POINTER and *ppFilter is left untouched.
    if (out == 0 || name_ptr == 0) return E_POINTER;
    const std::u16string name = read_u16_string(m, name_ptr);
    for (auto it = st->guest_filters.rbegin(); it != st->guest_filters.rend();
         ++it) {
        if (it->name != name) continue;  // case-sensitive (measured)
        (void)dshow_host().add_ref(m, it->filter);
        (void)m.write_ptr(out, it->filter);
        return com::S_OK;
    }
    (void)m.write_ptr(out, 0);
    return VFW_E_NOT_FOUND;
}

GuestAddr ComCoreManager::create_filter_enum(Marshaler& m, u32 sid,
                                             u32 position, u32 version) {
    const bool x64 = m.cpu_is_64bit();
    const GuestAddr ps = guest_pointer_size(m);
    GuestAddr& vtable = m_filter_enum_vtables[x64 ? 1 : 0];
    if (vtable == 0) {
        std::array<GuestAddr, kEnumFiltersMethodNames.size()> methods{};
        if (!load_filter_graph_thunks(m_thunks, kEnumFiltersMethodNames,
                                      methods)) {
            return 0;
        }
        const GuestAddr block =
            reserve_com_scratch(static_cast<GuestAddr>(methods.size()) * ps + 16u);
        if (block == 0) return 0;
        const GuestAddr aligned = (block + 15u) & ~GuestAddr{15u};
        write_vtable(m, aligned, methods.data(), methods.size());
        vtable = aligned;
    }
    const GuestAddr object = co_task_mem_alloc_guest(16);
    if (object == 0) return 0;
    (void)m.write_ptr(object, vtable);
    auto graph = m_filter_graphs.find(sid);
    if (graph == m_filter_graphs.end()) {
        co_task_mem_free_guest(object);
        return 0;
    }
    // Measured: each enumerator holds a reference on the graph.
    ++graph->second.ref_count;
    m_filter_enums[object] = FilterEnumState{sid, 1, position, version};
    return object;
}

u32 ComCoreManager::filter_graph_enum_filters(Marshaler& m) {
    // HRESULT EnumFilters(IEnumFilters** ppEnum)
    const GuestAddr this_ptr = m.arg_ptr(0);
    auto* st = find_filter_graph_by_iface(this_ptr);
    if (st == nullptr) return com::E_INVALIDARG;
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return E_POINTER;
    const GuestAddr object =
        create_filter_enum(m, find_filter_graph_id_by_iface(this_ptr), 0,
                           st->filter_list_version);
    if (object == 0) {
        (void)m.write_ptr(out, 0);
        return E_OUTOFMEMORY;
    }
    (void)m.write_ptr(out, object);
    return com::S_OK;
}

u32 ComCoreManager::filter_graph_enum_filters_method(Marshaler& m, u32 op) {
    const GuestAddr self = m.arg_ptr(0);
    auto it = m_filter_enums.find(self);
    if (it == m_filter_enums.end()) return com::E_INVALIDARG;
    FilterEnumState& en = it->second;
    auto graph = m_filter_graphs.find(en.filter_graph_id);
    if (graph == m_filter_graphs.end()) return E_FAIL;
    FilterGraphState& st = graph->second;
    const GuestAddr ps = guest_pointer_size(m);
    const u32 count = static_cast<u32>(st.guest_filters.size());
    // Enumeration order is most recently added first (measured).
    const auto filter_at = [&](u32 index) {
        return st.guest_filters[count - 1u - index].filter;
    };
    switch (op) {
    case 0: { // QueryInterface(riid, ppv)
        const GuestAddr riid = m.arg_ptr(1);
        const GuestAddr ppv = m.arg_ptr(2);
        if (ppv == 0) return E_POINTER;
        (void)m.write_ptr(ppv, 0);
        if (riid == 0) return com::E_INVALIDARG;
        auto iid = m.read_struct<Guid>(riid);
        if (iid.is_err()) return com::E_INVALIDARG;
        if (!guid_equal(iid.value(), IID_IUNKNOWN) &&
            !guid_equal(iid.value(), IID_IENUMFILTERS)) {
            return E_NOINTERFACE;
        }
        ++en.refs;
        (void)m.write_ptr(ppv, self);
        return com::S_OK;
    }
    case 1: // AddRef
        return ++en.refs;
    case 2: { // Release
        if (en.refs > 1) return --en.refs;
        const u32 sid = en.filter_graph_id;
        m_filter_enums.erase(it);
        co_task_mem_free_guest(self);
        (void)release_filter_graph_ref(m, sid);
        return 0;
    }
    case 3: { // Next(ULONG cFilters, IBaseFilter** ppFilter, ULONG* pcFetched)
        const u32 wanted = m.arg32(1);
        const GuestAddr out = m.arg_ptr(2);
        const GuestAddr fetched = m.arg_ptr(3);
        // Measured: NULL ppFilter is E_POINTER (even for 0 filters); more
        // than one filter without pcFetched is E_INVALIDARG.
        if (out == 0) return E_POINTER;
        if (wanted > 1 && fetched == 0) return com::E_INVALIDARG;
        if (en.version != st.filter_list_version) {
            if (fetched != 0) (void)m.write_u32(fetched, 0);
            return VFW_E_ENUM_OUT_OF_SYNC;
        }
        u32 got = 0;
        while (got < wanted && en.position < count) {
            const GuestAddr filter = filter_at(en.position++);
            (void)dshow_host().add_ref(m, filter);
            (void)m.write_ptr(out + static_cast<GuestAddr>(got) * ps, filter);
            ++got;
        }
        if (fetched != 0) (void)m.write_u32(fetched, got);
        return got == wanted ? com::S_OK : com::S_FALSE;
    }
    case 4: { // Skip(ULONG cFilters)
        if (en.version != st.filter_list_version) return VFW_E_ENUM_OUT_OF_SYNC;
        const u32 skip = m.arg32(1);
        const u32 left = count - std::min(en.position, count);
        if (skip <= left) {
            en.position += skip;
            return com::S_OK;
        }
        en.position = count;
        return com::S_FALSE;
    }
    case 5: // Reset
        en.position = 0;
        en.version = st.filter_list_version;
        return com::S_OK;
    case 6: { // Clone(IEnumFilters** ppEnum)
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) return E_POINTER;
        // Measured on Windows 11 x86 and x64: cloning an out-of-sync
        // enumerator returns S_OK and leaves *ppEnum unwritten.
        if (en.version != st.filter_list_version) return com::S_OK;
        const u32 sid = en.filter_graph_id;
        const u32 position = en.position;
        const u32 version = en.version;
        const GuestAddr clone = create_filter_enum(m, sid, position, version);
        if (clone == 0) return E_OUTOFMEMORY;
        (void)m.write_ptr(out, clone);
        return com::S_OK;
    }
    default:
        return E_NOTIMPL;
    }
}

} // namespace vitreo::winapi
