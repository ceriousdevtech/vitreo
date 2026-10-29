// SPDX-License-Identifier: LGPL-2.1-only

/// @file mfplay.cpp
/// @brief mfplay.dll exports, MFPCreateMediaPlayer, the guest object cells and
/// the event-delivery machinery. See mfplay.hpp for the contract sources.

#include "winapi/media/mfplay/mfplay_internal.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace vitreo::winapi {

using namespace mfplay;

namespace {

constexpr u32 kCell = 16;
constexpr u32 kEventBuffer = 64;  // largest event struct: MFP_MF_EVENT, 56 bytes on x64
constexpr u32 kChunk = 0x10000;

// x86 stdcall bytes: `this` plus every parameter (SDK mfplay.h prototypes).
constexpr std::array<u16, PLAYER_SLOT_COUNT> kPlayerBytes = {
    12, 4, 4, 4, 4, 4, 4,   // QI AddRef Release Play Pause Stop FrameStep
    12, 12, 12,             // SetPosition GetPosition GetDuration
    8, 8, 16, 8,            // SetRate GetRate GetSupportedRates GetState
    20, 20, 8, 4, 8,        // CreateMediaItemFromURL/Object SetMediaItem ClearMediaItem GetMediaItem
    8, 8, 8, 8, 8, 8,       // Get/SetVolume Get/SetBalance Get/SetMute
    12, 12, 8, 8, 8, 8,     // GetNativeVideoSize GetIdealVideoSize Set/GetVideoSourceRect Set/GetAspectRatioMode
    8, 4, 8, 8,             // GetVideoWindow UpdateVideo Set/GetBorderColor
    12, 8, 4, 4,            // InsertEffect RemoveEffect RemoveAllEffects Shutdown
};
constexpr std::array<u16, ITEM_SLOT_COUNT> kItemBytes = {
    12, 4, 4, 8, 8, 8, 8, 8,  // QI AddRef Release GetMediaPlayer GetURL GetObject Get/SetUserData
    20, 20, 12, 12, 8,        // Get/SetStartStopPosition HasVideo HasAudio IsProtected
    12, 8, 12, 12, 16, 12,    // GetDuration GetNumberOfStreams Get/SetStreamSelection GetStreamAttribute GetPresentationAttribute
    8, 12, 8,                 // GetCharacteristics SetStreamSink GetMetadata
};

u64 read_guest_ptr(GuestMemory& mem, bool is64, GuestAddr at) {
    if (is64) return mem.read_u64(at).value_or(0);
    return mem.read_u32(at).value_or(0);
}

void write_guest_ptr(GuestMemory& mem, bool is64, GuestAddr at, u64 value) {
    if (is64) (void)mem.write_u64(at, value);
    else (void)mem.write_u32(at, static_cast<u32>(value));
}

std::u16string utf8_to_utf16(const std::string& s) {
    std::u16string out;
    for (usize i = 0; i < s.size();) {
        const u8 c = static_cast<u8>(s[i]);
        u32 cp = c;
        usize n = 1;
        if (c >= 0xF0 && i + 3 < s.size() + 0) { cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) | ((s[i + 2] & 0x3Fu) << 6) | (s[i + 3] & 0x3Fu); n = 4; }
        else if (c >= 0xE0 && i + 2 < s.size()) { cp = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu); n = 3; }
        else if (c >= 0xC0 && i + 1 < s.size()) { cp = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3Fu); n = 2; }
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// A URL's scheme (lower case), or empty for a plain path. A drive letter
/// ("C:") is a path, not a one-letter scheme.
std::string url_scheme(const std::string& url) {
    const auto colon = url.find(':');
    if (colon == std::string::npos || colon < 2) return {};
    if (!std::isalpha(static_cast<unsigned char>(url[0]))) return {};
    std::string scheme;
    for (usize i = 0; i < colon; ++i) {
        const char c = url[i];
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '-' && c != '.') {
            return {};
        }
        scheme.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return scheme;
}

/// file: URL -> Windows path (RFC 8089 forms: file:///C:/x, file://host/share/x,
/// file:C:/x), percent-decoded, with backslashes.
std::string file_url_to_path(const std::string& url) {
    std::string rest = url.substr(5);  // after "file:"
    std::string host;
    if (rest.rfind("//", 0) == 0) {
        rest = rest.substr(2);
        const auto slash = rest.find('/');
        host = rest.substr(0, slash);
        rest = slash == std::string::npos ? std::string{} : rest.substr(slash);
    }
    std::string decoded;
    for (usize i = 0; i < rest.size(); ++i) {
        if (rest[i] == '%' && i + 2 < rest.size() && hex_value(rest[i + 1]) >= 0 &&
            hex_value(rest[i + 2]) >= 0) {
            decoded.push_back(static_cast<char>(hex_value(rest[i + 1]) * 16 + hex_value(rest[i + 2])));
            i += 2;
        } else {
            decoded.push_back(rest[i] == '/' ? '\\' : rest[i]);
        }
    }
    // "\C:\x" -> "C:\x"
    if (decoded.size() >= 3 && decoded[0] == '\\' && std::isalpha(static_cast<unsigned char>(decoded[1])) &&
        (decoded[2] == ':' || decoded[2] == '|')) {
        decoded = decoded.substr(1);
        decoded[1] = ':';
    }
    if (!host.empty() && host != "localhost") return "\\\\" + host + decoded;
    return decoded;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction, names
// ---------------------------------------------------------------------------

MfPlayManager::MfPlayManager() = default;

MfPlayManager::~MfPlayManager() {
    {
        std::lock_guard<std::mutex> lock(m_service_mutex);
        m_stopping = true;
    }
    m_service_wake.notify_all();
    if (m_service_thread.joinable()) m_service_thread.join();
    if (m_clock_thread.joinable()) m_clock_thread.join();
    for (auto& w : m_workers) {
        if (w.joinable()) w.join();
    }
}

void MfPlayManager::set_host(Host host) { m_host = std::move(host); }

std::string MfPlayManager::player_thunk_name(u32 slot) {
    return "Vitreo_IMFPMediaPlayer_" + std::to_string(slot);
}
std::string MfPlayManager::item_thunk_name(u32 slot) {
    return "Vitreo_IMFPMediaItem_" + std::to_string(slot);
}
u16 MfPlayManager::player_slot_stdcall_bytes(u32 slot) {
    return slot < PLAYER_SLOT_COUNT ? kPlayerBytes[slot] : u16{0};
}
u16 MfPlayManager::item_slot_stdcall_bytes(u32 slot) {
    return slot < ITEM_SLOT_COUNT ? kItemBytes[slot] : u16{0};
}

usize MfPlayManager::live_player_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_players.size();
}
usize MfPlayManager::live_item_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_items.size();
}

// ---------------------------------------------------------------------------
// Guest cells
// ---------------------------------------------------------------------------

GuestAddr MfPlayManager::alloc_block(u32 size) {
    auto& free_list = m_free_blocks[size];
    if (!free_list.empty()) {
        const GuestAddr a = free_list.back();
        free_list.pop_back();
        return a;
    }
    if (m_chunk == 0 || m_chunk_used + size > kChunk) {
        if (!m_host.alloc_guest) return 0;
        m_chunk = m_host.alloc_guest(kChunk);
        m_chunk_used = 0;
        if (m_chunk == 0) return 0;
    }
    const GuestAddr a = m_chunk + m_chunk_used;
    m_chunk_used += size;
    return a;
}

void MfPlayManager::free_block(GuestAddr addr, u32 size) {
    if (addr != 0) m_free_blocks[size].push_back(addr);
}

bool MfPlayManager::ensure_vtables(bool is64) {
    if (m_player_vtable != 0 && m_item_vtable != 0) return true;
    if (m_thunks == nullptr || m_memory == nullptr) return false;
    m_is64 = is64;
    const u32 ptr = is64 ? 8u : 4u;
    const auto build = [&](u32 count, auto name) -> GuestAddr {
        const GuestAddr vt = alloc_block(count * ptr);
        if (vt == 0) return 0;
        for (u32 slot = 0; slot < count; ++slot) {
            auto addr = m_thunks->get_thunk_address("mfplay.dll", name(slot));
            if (addr.is_err() || addr.value() == 0) return 0;
            write_guest_ptr(*m_memory, is64, vt + slot * ptr, addr.value());
        }
        return vt;
    };
    m_player_vtable = build(PLAYER_SLOT_COUNT, &MfPlayManager::player_thunk_name);
    m_item_vtable = build(ITEM_SLOT_COUNT, &MfPlayManager::item_thunk_name);
    return m_player_vtable != 0 && m_item_vtable != 0;
}

MfPlayManager::PlayerPtr MfPlayManager::find_player(GuestAddr self) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_players.find(self);
    return it == m_players.end() ? nullptr : it->second;
}

MfPlayManager::ItemPtr MfPlayManager::find_item(GuestAddr self) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_items.find(self);
    return it == m_items.end() ? nullptr : it->second;
}

MfPlayManager::PlayerPtr MfPlayManager::find_player_by_window(u64 hwnd) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [cell, p] : m_players) {
        if (p->event_hwnd == hwnd) return p;
    }
    return nullptr;
}

MfPlayManager::ItemPtr MfPlayManager::new_item(const PlayerPtr& owner, u64 user_data) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const GuestAddr cell = alloc_block(kCell);
    if (cell == 0) return nullptr;
    write_guest_ptr(*m_memory, m_is64, cell, m_item_vtable);
    auto item = std::make_shared<Item>();
    item->cell = cell;
    item->owner = owner;
    item->user_data = user_data;
    m_items[cell] = item;
    return item;
}

void MfPlayManager::destroy_item(const ItemPtr& item) {
    std::unique_ptr<audio::IMediaPlayer> media;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_items.erase(item->cell);
        free_block(item->cell, kCell);
        item->cell = 0;
        media = std::move(item->media);
    }
    if (media) media->stop();
}

void MfPlayManager::item_add_ref(const ItemPtr& item) {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++item->refs;
}

void MfPlayManager::item_release(const ItemPtr& item) {
    bool last = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (item->refs == 0) return;
        last = --item->refs == 0;
    }
    if (last) destroy_item(item);
}

// ---------------------------------------------------------------------------
// Opening a URL
// ---------------------------------------------------------------------------

u32 MfPlayManager::open_url(Item& item, const std::string& url) {
    item.url_utf8 = url;
    item.url = utf8_to_utf16(url);
    std::string path = url;
    const std::string scheme = url_scheme(url);
    if (!scheme.empty()) {
        if (scheme != "file") {
            // Learn CreateMediaItemFromURL: MF_E_UNSUPPORTED_SCHEME for a
            // protocol the player does not open; measured for an unknown one.
            if (scheme == "http" || scheme == "https") {
                MFPLAY_UNIMPLEMENTED("CreateMediaItemFromURL", "network (http/https) URLs are not opened");
            }
            return MF_E_UNSUPPORTED_SCHEME;
        }
        path = file_url_to_path(url);
    }
    if (!m_host.resolve_path) return HRESULT_PATH_NOT_FOUND;
    const auto host = m_host.resolve_path(path);
    if (!host) return HRESULT_PATH_NOT_FOUND;
    std::error_code ec;
    const std::filesystem::path fs_path(*host);
    if (!std::filesystem::is_regular_file(fs_path, ec)) {
        // The file byte stream opens with CreateFile semantics: a missing
        // file is ERROR_FILE_NOT_FOUND (measured), a missing directory
        // ERROR_PATH_NOT_FOUND.
        if (std::filesystem::exists(fs_path, ec)) return MF_E_UNSUPPORTED_BYTESTREAM_TYPE;
        return std::filesystem::is_directory(fs_path.parent_path(), ec) ? HRESULT_FILE_NOT_FOUND
                                                                       : HRESULT_PATH_NOT_FOUND;
    }
    item.file_size = static_cast<u64>(std::filesystem::file_size(fs_path, ec));
    // No byte-stream handler takes the file: MF_E_UNSUPPORTED_BYTESTREAM_TYPE
    // (measured for a file that is not media).
    if (!m_host.create_media) return MF_E_UNSUPPORTED_BYTESTREAM_TYPE;
    auto media = m_host.create_media();
    if (!media || !media->open(*host)) return MF_E_UNSUPPORTED_BYTESTREAM_TYPE;
    item.has_audio = media->has_audio();
    item.has_video = media->has_video();
    if (!item.has_audio && !item.has_video) return MF_E_UNSUPPORTED_BYTESTREAM_TYPE;
    media->native_video_size(item.video_width, item.video_height);
    item.duration = media->duration_100ns();
    if (item.has_video) item.stream_is_video.push_back(true);
    if (item.has_audio) item.stream_is_video.push_back(false);
    item.selected.assign(item.stream_is_video.size(), true);
    item.media = std::move(media);
    return S_OK;
}

// ---------------------------------------------------------------------------
// MFPCreateMediaPlayer
// ---------------------------------------------------------------------------

void MfPlayManager::create_media_player(Marshaler& m) {
    const GuestAddr url = m.arg_ptr(0);
    const bool start = m.arg32(1) != 0;
    const u32 options = m.arg32(2);
    const GuestAddr callback = m.arg_ptr(3);
    const u64 hwnd = m.arg_ptr(4);
    const GuestAddr out = m.arg_ptr(5);

    // Learn: ppMediaPlayer may be NULL only for fire-and-forget playback (a
    // URL and fStartPlayback TRUE); measured E_POINTER otherwise.
    if (out == 0 && !(url != 0 && start)) {
        m.set_return(E_POINTER);
        return;
    }
    if (out != 0) (void)m.write_ptr(out, 0);
    // Learn: fStartPlayback must be FALSE without a URL; measured E_INVALIDARG.
    if (url == 0 && start) {
        m.set_return(E_INVALIDARG);
        return;
    }
    std::string url_utf8;
    if (url != 0) {
        auto s = m.read_wide_string(url);
        if (s.is_err()) {
            m.set_return(E_POINTER);
            return;
        }
        url_utf8 = s.value();
    }

    auto p = std::make_shared<Player>();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!ensure_vtables(m.cpu_is_64bit())) {
            m.set_return(E_OUTOFMEMORY);
            return;
        }
        p->cell = alloc_block(kCell);
        p->event_buffer = alloc_block(kEventBuffer);
        if (p->cell == 0 || p->event_buffer == 0) {
            free_block(p->cell, kCell);
            free_block(p->event_buffer, kEventBuffer);
            m.set_return(E_OUTOFMEMORY);
            return;
        }
        write_guest_ptr(*m_memory, m_is64, p->cell, m_player_vtable);
        m_players[p->cell] = p;
    }
    // Every bit is accepted (measured, including undefined ones). NO_MMCSS and
    // NO_REMOTE_DESKTOP_OPTIMIZATION select scheduling and remoting features
    // that do not exist on this host, so they change nothing observable.
    p->options = options;
    p->free_threaded = (options & MFP_OPTION_FREE_THREADED_CALLBACK) != 0;
    p->video_hwnd = hwnd;
    p->callback = callback;
    p->creator_thread = m_host.current_thread_id ? m_host.current_thread_id() : 0;

    // The default delivery mode owns one hidden window on this thread, with
    // or without a callback (measured: one thread window more, none with
    // MFP_OPTION_FREE_THREADED_CALLBACK).
    if (!p->free_threaded) {
        auto proc = m_thunks->get_thunk_address("mfplay.dll", wndproc_thunk_name());
        if (proc.is_ok() && m_host.create_event_window) {
            p->event_hwnd = m_host.create_event_window(proc.value());
        }
        if (p->event_hwnd == 0) {
            p->callback = 0;
            release_player_cell(p);
            m.set_return(E_FAIL);
            return;
        }
    }
    if (callback != 0 && m_host.call_guest) {
        // The player holds its callback until Shutdown (measured 1 -> 2 -> 1).
        const u64 vt = read_guest_ptr(*m_memory, m_is64, callback);
        const u64 add_ref = read_guest_ptr(*m_memory, m_is64, vt + (m_is64 ? 8u : 4u));
        const u64 args[1] = {callback};
        (void)m_host.call_guest(add_ref, args, 1);
    }

    if (url != 0) {
        ItemPtr item = new_item(p, 0);
        const u32 hr = item ? open_url(*item, url_utf8) : E_OUTOFMEMORY;
        if (hr != S_OK) {
            if (item) destroy_item(item);
            shutdown_player(p, false);
            release_player_cell(p);
            m.set_return(hr);
            return;
        }
        // Measured: fStartPlayback TRUE raises MEDIAITEM_SET then PLAY;
        // FALSE raises nothing and leaves the player empty.
        if (start) {
            (void)set_media_item(m, p, item);
            play(p);
        }
        item_release(item);
    }
    if (out != 0) {
        (void)m.write_ptr(out, p->cell);
    } else {
        p->fire_and_forget = true;
    }
    ensure_clock();
    m.set_return(S_OK);
}

void MfPlayManager::release_player_cell(const PlayerPtr& p) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_players.erase(p->cell);
    free_block(p->cell, kCell);
    free_block(p->event_buffer, kEventBuffer);
    p->cell = 0;
    p->event_buffer = 0;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void MfPlayManager::shutdown_player(const PlayerPtr& p, bool service_thread) {
    GuestAddr callback = 0;
    std::vector<Player::Effect> effects;
    ItemPtr current;
    u64 window = 0;
    u64 video = 0;
    bool same_thread = false;
    {
        std::lock_guard<std::recursive_mutex> lock(p->mutex);
        if (p->shut_down) return;
        p->shut_down = true;
        p->state = State::Shutdown;
        p->reported_state = State::Shutdown;
        p->window_queue.clear();
        current = std::move(p->current);
        p->current_visible = false;
        callback = p->callback;
        p->callback = 0;
        effects.swap(p->effects);
        window = p->event_hwnd;
        p->event_hwnd = 0;
        video = p->video_shown ? p->video_hwnd : 0;
        p->video_shown = false;
        same_thread = !service_thread && m_host.current_thread_id &&
                      m_host.current_thread_id() == p->creator_thread;
    }
    if (current) {
        if (current->media) current->media->stop();
        item_release(current);
    }
    if (video != 0 && m_host.hide_video) m_host.hide_video(video);
    if (window != 0) {
        // DestroyWindow belongs to the window's thread.
        if (same_thread && m_host.destroy_window) m_host.destroy_window(window);
        else if (m_host.post_message) (void)m_host.post_message(window, DESTROY_WINDOW_MESSAGE, 0, 0);
    }
    const auto& call = service_thread ? m_host.call_guest_service : m_host.call_guest;
    if (!call || m_memory == nullptr) return;
    const auto release = [&](GuestAddr punk) {
        const u64 vt = read_guest_ptr(*m_memory, m_is64, punk);
        const u64 fn = read_guest_ptr(*m_memory, m_is64, vt + 2 * (m_is64 ? 8u : 4u));
        const u64 args[1] = {punk};
        (void)call(fn, args, 1);
    };
    for (const auto& e : effects) release(e.punk);
    if (callback != 0) release(callback);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

namespace {
/// Internal marker: a fire-and-forget player finished; shut it down on the
/// delivery thread (its callback is released through guest code).
constexpr auto kFinish = static_cast<EventType>(0xFFFFu);
} // namespace

void MfPlayManager::post_event(const PlayerPtr& p, Event ev) {
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    if (p->shut_down) {
        if (ev.owns_item_ref && ev.item) item_release(ev.item);
        return;
    }
    if (p->callback == 0 && ev.type != kFinish) {
        // No callback, no events (Learn); the state they carry still lands.
        p->reported_state = ev.state;
        if (ev.type == EventType::MediaItemSet && ev.hr == S_OK) p->current_visible = true;
        if (ev.owns_item_ref && ev.item) item_release(ev.item);
        return;
    }
    if (p->free_threaded) {
        {
            std::lock_guard<std::mutex> service(m_service_mutex);
            m_service_queue.emplace_back(p, std::make_shared<Event>(std::move(ev)));
            if (!m_service_thread.joinable() && !m_stopping) {
                m_service_thread = std::thread([this] { service_loop(); });
            }
        }
        m_service_wake.notify_one();
        return;
    }
    p->window_queue.push_back(std::move(ev));
    if (m_host.post_message) (void)m_host.post_message(p->event_hwnd, EVENT_WINDOW_MESSAGE, 0, 0);
}

void MfPlayManager::deliver(const PlayerPtr& p, const Event& ev, bool service_thread) {
    if (ev.type == kFinish) {
        shutdown_player(p, service_thread);
        release_player_cell(p);
        return;
    }
    GuestAddr callback = 0;
    GuestAddr buf = 0;
    GuestAddr self = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(p->mutex);
        if (p->shut_down) {
            if (ev.owns_item_ref && ev.item) item_release(ev.item);
            return;
        }
        callback = p->callback;
        buf = p->event_buffer;
        self = p->cell;
        p->reported_state = ev.state;
        if (ev.type == EventType::MediaItemSet && ev.hr == S_OK && ev.item == p->current) {
            p->current_visible = true;
        }
        ++p->refs;  // the header's pMediaPlayer stays valid through the call
    }
    const auto& call = service_thread ? m_host.call_guest_service : m_host.call_guest;
    if (callback != 0 && call && m_memory != nullptr) {
        GuestMemory& mem = *m_memory;
        const bool is64 = m_is64;
        const u32 ptr = is64 ? 8u : 4u;
        // SDK mfplay.h MFP_EVENT_HEADER {eEventType; hrEvent; pMediaPlayer;
        // eState; pPropertyStore} then the event's own members.
        const std::vector<Byte> zero(kEventBuffer, 0);
        (void)mem.write_bytes(buf, zero.data(), zero.size());
        (void)mem.write_u32(buf, static_cast<u32>(ev.type));
        (void)mem.write_u32(buf + 4, ev.hr);
        write_guest_ptr(mem, is64, buf + 8, self);
        (void)mem.write_u32(buf + 8 + ptr, static_cast<u32>(ev.state));
        const GuestAddr store = buf + (is64 ? 24u : 16u);
        write_guest_ptr(mem, is64, store, 0);
        const GuestAddr body = store + ptr;
        const GuestAddr item_cell = ev.item ? ev.item->cell : 0;
        if (ev.item) item_add_ref(ev.item);  // alive for the duration of the call
        switch (ev.type) {
            case EventType::Error:
                break;
            case EventType::RateSet:
                write_guest_ptr(mem, is64, body, item_cell);
                (void)mem.write_u32(body + ptr, static_cast<u32>(ev.extra));
                break;
            case EventType::MediaItemCreated:
                write_guest_ptr(mem, is64, body, item_cell);
                write_guest_ptr(mem, is64, body + ptr, ev.extra);
                break;
            default:
                write_guest_ptr(mem, is64, body, item_cell);
                break;
        }
        const u64 vt = read_guest_ptr(mem, is64, callback);
        const u64 fn = read_guest_ptr(mem, is64, vt + CALLBACK_SLOT_ON_EVENT * ptr);
        const u64 args[2] = {callback, buf};
        (void)call(fn, args, 2);
        if (ev.item) item_release(ev.item);
    }
    // MEDIAITEM_CREATED hands its item to the callback: the reference the
    // creation made is dropped once the callback returns, so an application
    // keeps the item by AddRef or SetMediaItem (the Learn samples' pattern).
    if (ev.owns_item_ref && ev.item) item_release(ev.item);
    bool last = false;
    {
        std::lock_guard<std::recursive_mutex> lock(p->mutex);
        last = --p->refs == 0;
    }
    if (last) {
        shutdown_player(p, service_thread);
        release_player_cell(p);
    }
}

void MfPlayManager::drain_window_queue(const PlayerPtr& p) {
    for (;;) {
        Event ev;
        {
            std::lock_guard<std::recursive_mutex> lock(p->mutex);
            // OnMediaPlayerEvent is never re-entered (Learn): a nested message
            // loop inside the callback leaves the queue to the outer drain.
            if (p->in_callback || p->window_queue.empty()) return;
            ev = std::move(p->window_queue.front());
            p->window_queue.pop_front();
            p->in_callback = true;
        }
        deliver(p, ev, false);
        std::lock_guard<std::recursive_mutex> lock(p->mutex);
        p->in_callback = false;
    }
}

void MfPlayManager::service_loop() {
    std::unique_lock<std::mutex> lock(m_service_mutex);
    for (;;) {
        m_service_wake.wait(lock, [&] { return m_stopping || !m_service_queue.empty(); });
        if (m_stopping) return;
        auto [p, ev] = std::move(m_service_queue.front());
        m_service_queue.pop_front();
        m_service_busy = true;
        lock.unlock();
        deliver(p, *ev, true);
        lock.lock();
        m_service_busy = false;
        if (m_service_queue.empty()) m_service_idle.notify_all();
    }
}

void MfPlayManager::drain_service_queue() {
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        workers.swap(m_workers);
    }
    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }
    std::unique_lock<std::mutex> lock(m_service_mutex);
    m_service_idle.wait(lock, [&] {
        return m_stopping || (m_service_queue.empty() && !m_service_busy);
    });
}

u64 MfPlayManager::event_window_proc(Marshaler& m) {
    const u64 hwnd = m.arg_ptr(0);
    const u32 msg = m.arg32(1);
    if (msg == EVENT_WINDOW_MESSAGE) {
        if (auto p = find_player_by_window(hwnd)) drain_window_queue(p);
        return 0;
    }
    if (msg == DESTROY_WINDOW_MESSAGE) {
        if (m_host.destroy_window) m_host.destroy_window(hwnd);
        return 0;
    }
    if (!m_host.def_window_proc) return 0;
    return m_host.def_window_proc(hwnd, msg, m.arg_ptr(2), m.arg_ptr(3));
}

// ---------------------------------------------------------------------------
// Playback clock: end of media and video frames
// ---------------------------------------------------------------------------

void MfPlayManager::ensure_clock() {
    std::lock_guard<std::mutex> lock(m_service_mutex);
    if (!m_clock_enabled || m_clock_thread.joinable() || m_stopping || !m_host.create_media) return;
    m_clock_thread = std::thread([this] {
        for (;;) {
            {
                std::lock_guard<std::mutex> l(m_service_mutex);
                if (m_stopping) return;
            }
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
}

void MfPlayManager::tick() {
    std::vector<PlayerPtr> players;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [cell, p] : m_players) players.push_back(p);
    }
    for (const auto& p : players) {
        std::lock_guard<std::recursive_mutex> lock(p->mutex);
        if (p->shut_down || !p->current || !p->current->media) continue;
        Item& item = *p->current;
        if (p->state == State::Playing) {
            bool ended = item.media->take_completed();
            if (!ended && item.stop && item.media->position_100ns() >= *item.stop) ended = true;
            if (ended) {
                // MF stops at the end of the presentation and rewinds to the
                // item's start; Play starts it again.
                item.media->stop();
                (void)item.media->seek_100ns(item.start.value_or(0));
                p->state = State::Stopped;
                post_event(p, Event{EventType::PlaybackEnded, S_OK, State::Stopped, p->current, 0});
                if (p->fire_and_forget) post_event(p, Event{kFinish, S_OK, State::Stopped, nullptr, 0});
                continue;
            }
        }
        if (p->state == State::Playing || p->state == State::Paused) present_frame(p.operator*(), false);
    }
}

void MfPlayManager::present_frame(Player& p, bool force) {
    if (!p.current || !p.current->media || !p.current->has_video || p.video_hwnd == 0 ||
        !m_host.present_video) {
        return;
    }
    Item& item = *p.current;
    // The video stream is index 0 when present (open_url).
    if (item.selected.empty() || !item.selected[0]) return;
    std::vector<u8> rgba;
    u32 w = 0, h = 0;
    u64 frame = 0;
    if (!item.media->copy_video_frame(rgba, w, h, frame) || w == 0 || h == 0) return;
    if (!force && frame == p.last_frame && p.video_shown) return;
    p.last_frame = frame;

    // Source rectangle (normalized, SetVideoSourceRect).
    const auto edge = [](float v, u32 size) {
        return static_cast<u32>(std::clamp(std::lround(v * static_cast<float>(size)), 0L,
                                           static_cast<long>(size)));
    };
    const u32 x0 = edge(p.source_rect[0], w), y0 = edge(p.source_rect[1], h);
    const u32 x1 = edge(p.source_rect[2], w), y1 = edge(p.source_rect[3], h);
    if (x1 <= x0 || y1 <= y0) return;
    u32 cw = x1 - x0, ch = y1 - y0;

    // MFVideoARMode_PreservePicture letterboxes in the border colour; None
    // stretches the source rectangle over the whole client area.
    u32 out_w = cw, out_h = ch;
    i32 client_w = 0, client_h = 0;
    const bool letterbox = (p.aspect_mode & MFVideoARMode_PreservePicture) != 0 &&
                           m_host.client_size && m_host.client_size(p.video_hwnd, client_w, client_h) &&
                           client_w > 0 && client_h > 0;
    if (letterbox) {
        const double client_aspect = static_cast<double>(client_w) / client_h;
        const double source_aspect = static_cast<double>(cw) / ch;
        if (source_aspect > client_aspect) out_h = static_cast<u32>(std::lround(cw / client_aspect));
        else out_w = static_cast<u32>(std::lround(ch * client_aspect));
        out_w = std::max(out_w, cw);
        out_h = std::max(out_h, ch);
    }
    std::vector<u8> out(static_cast<usize>(out_w) * out_h * 4);
    const u8 r = static_cast<u8>(p.border_color & 0xFF);
    const u8 g = static_cast<u8>((p.border_color >> 8) & 0xFF);
    const u8 b = static_cast<u8>((p.border_color >> 16) & 0xFF);
    for (usize i = 0; i < out.size(); i += 4) {
        out[i] = r; out[i + 1] = g; out[i + 2] = b; out[i + 3] = 0xFF;
    }
    const u32 ox = (out_w - cw) / 2, oy = (out_h - ch) / 2;
    for (u32 row = 0; row < ch; ++row) {
        std::memcpy(&out[(static_cast<usize>(oy + row) * out_w + ox) * 4],
                    &rgba[(static_cast<usize>(y0 + row) * w + x0) * 4], static_cast<usize>(cw) * 4);
    }
    m_host.present_video(p.video_hwnd, out.data(), static_cast<i32>(out_w), static_cast<i32>(out_h));
    p.video_shown = true;
}

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------

DllExports create_mfplay_thunks(MfPlayManager& mgr) {
    DllExports exports;

    // HRESULT MFPCreateMediaPlayer(LPCWSTR, BOOL, MFP_CREATION_OPTIONS,
    //     IMFPMediaPlayerCallback*, HWND, IMFPMediaPlayer**)  -- x86 @24
    exports.entries.push_back(ExportEntry{
        "MFPCreateMediaPlayer", 0, [&mgr](Marshaler& m) { mgr.create_media_player(m); }, 24});

    // Undocumented (no SDK prototype); the WOW64 DLL pops 28 bytes.
    exports.entries.push_back(ExportEntry{
        "MFPCreateMediaPlayerEx", 0,
        [](Marshaler& m) {
            MFPLAY_UNIMPLEMENTED("MFPCreateMediaPlayerEx", "undocumented export");
            m.set_return(E_NOTIMPL);
        },
        28});

    // COM-server entry points. mfplay registers no class Vitreo provides:
    // DllGetClassObject answers CLASS_E_CLASSNOTAVAILABLE with *ppv NULL
    // (measured for an unknown CLSID), DllCanUnloadNow S_OK.
    exports.entries.push_back(ExportEntry{
        "DllCanUnloadNow", 0, [](Marshaler& m) { m.set_return(S_OK); }, 0});
    exports.entries.push_back(ExportEntry{
        "DllGetClassObject", 0,
        [](Marshaler& m) {
            if (const GuestAddr ppv = m.arg_ptr(2); ppv != 0) (void)m.write_ptr(ppv, 0);
            m.set_return(CLASS_E_CLASSNOTAVAILABLE);
        },
        12});
    for (const char* name : {"DllRegisterServer", "DllUnregisterServer"}) {
        exports.entries.push_back(ExportEntry{
            name, 0,
            [name](Marshaler& m) {
                if (std::string_view(name) == "DllRegisterServer") {
                    MFPLAY_UNIMPLEMENTED("DllRegisterServer", "no classes to register");
                } else {
                    MFPLAY_UNIMPLEMENTED("DllUnregisterServer", "no classes to unregister");
                }
                m.set_return(SELFREG_E_CLASS);
            },
            0});
    }

    // LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM)
    exports.entries.push_back(ExportEntry{
        MfPlayManager::wndproc_thunk_name(), 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.event_window_proc(m)); }, 16});

    for (u32 slot = 0; slot < PLAYER_SLOT_COUNT; ++slot) {
        exports.entries.push_back(ExportEntry{
            MfPlayManager::player_thunk_name(slot), 0,
            [&mgr, slot](Marshaler& m) { m.set_return(mgr.dispatch_player(slot, m)); },
            MfPlayManager::player_slot_stdcall_bytes(slot)});
    }
    for (u32 slot = 0; slot < ITEM_SLOT_COUNT; ++slot) {
        exports.entries.push_back(ExportEntry{
            MfPlayManager::item_thunk_name(slot), 0,
            [&mgr, slot](Marshaler& m) { m.set_return(mgr.dispatch_item(slot, m)); },
            MfPlayManager::item_slot_stdcall_bytes(slot)});
    }
    return exports;
}

} // namespace vitreo::winapi
