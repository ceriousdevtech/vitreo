// SPDX-License-Identifier: LGPL-2.1-only
#include "winapi/graphics/common/live_diag.hpp"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mach/mach_time.h>
#include <unistd.h>

namespace vitreo::winapi::live_diag {

namespace {

/// mach_approximate_time ticks in one second.
u64 one_second_ticks() noexcept {
    static const u64 ticks = [] {
        mach_timebase_info_data_t timebase{};
        (void)mach_timebase_info(&timebase);
        if (timebase.numer == 0) return u64{24'000'000};
        return static_cast<u64>(1'000'000'000ull * timebase.denom /
                                timebase.numer);
    }();
    return ticks;
}

struct Sink {
    std::mutex mutex;
    LogSink sink{nullptr};
    void* user{nullptr};
};

Sink& sink_state() noexcept {
    // Never destroyed: a diagnostic must outlive the statics that log from
    // their destructors.
    static Sink* const state = new Sink();
    return *state;
}

}  // namespace

MarkerFile::MarkerFile(std::string path) : m_path(std::move(path)) {}

bool MarkerFile::present() noexcept {
    // The commpage's approximate clock (a load, no counter read): this runs
    // several times per draw, and mach_absolute_time's ISB + counter read was
    // itself 2% of UE's RHI thread (d3d11_skip_stats_enabled).
    const u64 now = mach_approximate_time();
    u64 due = m_next_probe.load(std::memory_order_relaxed);
    if (now >= due &&
        m_next_probe.compare_exchange_strong(due, now + one_second_ticks(),
                                             std::memory_order_relaxed)) {
        probe(now);
    }
    return m_present.load(std::memory_order_relaxed);
}

void MarkerFile::probe(u64 /*now*/) noexcept {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        path = m_path;
    }
    std::string contents;
    bool exists = false;
    if (std::FILE* file = std::fopen(path.c_str(), "rb")) {
        exists = true;
        std::array<char, 4096> text{};
        const usize got = std::fread(text.data(), 1, text.size(), file);
        contents.assign(text.data(), got);
        std::fclose(file);
    }
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_path != path) return;  // set_path ran meanwhile; it re-arms
        changed = exists && (!m_present.load(std::memory_order_relaxed) ||
                             contents != m_contents);
        m_contents = std::move(contents);
        m_present.store(exists, std::memory_order_relaxed);
    }
    if (changed) m_generation.fetch_add(1, std::memory_order_relaxed);
}

std::string MarkerFile::contents() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_contents;
}

u64 MarkerFile::generation() const noexcept {
    return m_generation.load(std::memory_order_relaxed);
}

std::string MarkerFile::path() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_path;
}

void MarkerFile::set_path(std::string path) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_path = std::move(path);
        m_contents.clear();
        m_present.store(false, std::memory_order_relaxed);
    }
    m_generation.fetch_add(1, std::memory_order_relaxed);
    m_next_probe.store(0, std::memory_order_relaxed);
}

MarkerFile& caps_marker() noexcept {
    static MarkerFile* const marker = new MarkerFile("/tmp/vitreo-d3d11-caps");
    return *marker;
}

MarkerFile& draw_dump_marker() noexcept {
    static MarkerFile* const marker = new MarkerFile("/tmp/vitreo-draw-dump");
    return *marker;
}

void set_log_sink(LogSink sink, void* user) noexcept {
    Sink& state = sink_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.sink = sink;
    state.user = user;
}

void emit(std::string_view tag, std::string_view text) {
    const std::string line = strprintf("[%.*s] pid=%d %.*s",
                                       static_cast<int>(tag.size()), tag.data(),
                                       static_cast<int>(getpid()),
                                       static_cast<int>(text.size()),
                                       text.data());
    Sink& state = sink_state();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.sink != nullptr) {
            state.sink(state.user, line);
            return;
        }
    }
    // One fprintf of the whole line keeps it intact among the other threads
    // and processes writing the same log.
    std::fprintf(stderr, "%s\n", line.c_str());
    std::fflush(stderr);
}

std::string strprintf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    const int length = std::vsnprintf(nullptr, 0, format, copy);
    va_end(copy);
    std::string out;
    if (length > 0) {
        out.resize(static_cast<usize>(length) + 1);
        std::vsnprintf(out.data(), out.size(), format, args);
        out.resize(static_cast<usize>(length));
    }
    va_end(args);
    return out;
}

std::string hex_words(std::span<const u8> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2 + bytes.size() / 4);
    for (usize i = 0; i < bytes.size(); ++i) {
        if (i != 0 && i % 4 == 0) out.push_back(' ');
        out.push_back(kDigits[bytes[i] >> 4]);
        out.push_back(kDigits[bytes[i] & 0xF]);
    }
    return out;
}

namespace caps {

std::string_view feature_name(u32 feature) noexcept {
    // d3d11.h (Windows Kits 10.0.26100.0), enum D3D11_FEATURE, in order.
    static constexpr std::array<std::string_view, 22> kNames = {
        "THREADING",
        "DOUBLES",
        "FORMAT_SUPPORT",
        "FORMAT_SUPPORT2",
        "D3D10_X_HARDWARE_OPTIONS",
        "D3D11_OPTIONS",
        "ARCHITECTURE_INFO",
        "D3D9_OPTIONS",
        "SHADER_MIN_PRECISION_SUPPORT",
        "D3D9_SHADOW_SUPPORT",
        "D3D11_OPTIONS1",
        "D3D9_SIMPLE_INSTANCING_SUPPORT",
        "MARKER_SUPPORT",
        "D3D9_OPTIONS1",
        "D3D11_OPTIONS2",
        "D3D11_OPTIONS3",
        "GPU_VIRTUAL_ADDRESS_SUPPORT",
        "D3D11_OPTIONS4",
        "SHADER_CACHE",
        "D3D11_OPTIONS5",
        "DISPLAYABLE",
        "D3D11_OPTIONS6",
    };
    return feature < kNames.size() ? kNames[feature] : std::string_view("?");
}

std::string format_create_device(std::string_view api, u32 driver_type,
                                 u32 flags,
                                 std::span<const u32> requested_levels,
                                 bool levels_given, u32 chosen_level, u32 hr) {
    std::string requested;
    if (!levels_given) {
        requested = "default";
    } else if (requested_levels.empty()) {
        requested = "none";
    } else {
        for (usize i = 0; i < requested_levels.size(); ++i) {
            if (i != 0) requested += ',';
            requested += strprintf("0x%x", requested_levels[i]);
        }
    }
    const std::string chosen =
        chosen_level == 0 ? std::string("none")
                          : strprintf("0x%x", chosen_level);
    return strprintf(
        "%.*s driver_type=%u flags=0x%x requested_fl=%s chosen_fl=%s "
        "hr=0x%08x",
        static_cast<int>(api.size()), api.data(), driver_type, flags,
        requested.c_str(), chosen.c_str(), hr);
}

std::string format_check_feature_support(u32 feature, u32 size, u32 hr,
                                         bool data_given,
                                         std::span<const u8> data) {
    std::string line = strprintf(
        "CheckFeatureSupport feature=%u name=%.*s size=%u hr=0x%08x", feature,
        static_cast<int>(feature_name(feature).size()),
        feature_name(feature).data(), size, hr);
    // FORMAT_SUPPORT / FORMAT_SUPPORT2: { DXGI_FORMAT InFormat; UINT
    // OutFormatSupport }.
    if ((feature == 2 || feature == 3) && data.size() >= 8) {
        u32 format = 0;
        u32 support = 0;
        std::memcpy(&format, data.data(), 4);
        std::memcpy(&support, data.data() + 4, 4);
        line += strprintf(" format=%u support=0x%08x", format, support);
    }
    line += data_given ? " data=" + hex_words(data) : std::string(" data=null");
    return line;
}

std::string format_check_format_support(u32 format, u32 hr, u32 mask) {
    return strprintf("CheckFormatSupport format=%u hr=0x%08x support=0x%08x",
                     format, hr, mask);
}

std::string format_check_multisample(std::string_view api, u32 format,
                                     u32 samples, bool has_flags, u32 flags,
                                     u32 hr, u32 levels) {
    std::string line = strprintf(
        "%.*s format=%u samples=%u", static_cast<int>(api.size()), api.data(),
        format, samples);
    if (has_flags) line += strprintf(" flags=0x%x", flags);
    line += strprintf(" hr=0x%08x levels=%u", hr, levels);
    return line;
}

std::string format_adapter_desc(const AdapterDesc& desc) {
    std::string line = strprintf(
        "Adapter %.*s description=\"%s\" vendor=0x%04x device=0x%04x "
        "subsys=0x%08x revision=%u dedicated_video=%llu dedicated_system=%llu "
        "shared_system=%llu luid=%08x:%08x",
        static_cast<int>(desc.api.size()), desc.api.data(),
        desc.description.c_str(), desc.vendor_id, desc.device_id,
        desc.subsystem_id, desc.revision,
        static_cast<unsigned long long>(desc.dedicated_video_memory),
        static_cast<unsigned long long>(desc.dedicated_system_memory),
        static_cast<unsigned long long>(desc.shared_system_memory),
        static_cast<u32>(desc.luid_high), desc.luid_low);
    if (desc.has_flags) line += strprintf(" flags=0x%x", desc.flags);
    if (desc.has_preemption) {
        line += strprintf(" graphics_preemption=%u compute_preemption=%u",
                          desc.graphics_preemption, desc.compute_preemption);
    }
    return line;
}

void log(std::string_view text) {
    if (!caps_marker().present()) return;
    emit(kTag, text);
}

}  // namespace caps

}  // namespace vitreo::winapi::live_diag
