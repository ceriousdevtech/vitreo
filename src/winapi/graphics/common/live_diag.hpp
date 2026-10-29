// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file live_diag.hpp
/// @brief Marker-file switches and the line formats of the graphics live-run
///        diagnostics (/tmp/vitreo-d3d11-caps, /tmp/vitreo-draw-dump).
///
/// A guest launched by a store client is several re-execs removed from the
/// shell and does not reliably inherit environment variables, so an
/// environment switch silently produces nothing for exactly the processes
/// worth diagnosing. A marker file always arrives: touch it before the guest
/// starts, or while it runs, and remove it to stop.
///
/// These are LOG-ONLY diagnostics: they change no behaviour, and with the
/// marker absent they cost two atomic loads and a clock read per use (the
/// d3d11_skip_stats_enabled() pattern: the marker is re-probed at most once a
/// second, never per call).
///
/// Every line is `[<tag>] pid=<pid> <text>` on stderr (the process's Vitreo
/// log), so one grep on the tag finds them and the pid separates the browser,
/// GPU and renderer processes of a multi-process guest that all log to the same place.

#include <vitreo/types.hpp>

#include <atomic>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

namespace vitreo::winapi::live_diag {

/// A diagnostic enabled by the existence of a file. present() is cheap enough
/// to call per draw.
class MarkerFile {
public:
    explicit MarkerFile(std::string path);

    /// Whether the file existed at the last probe; probes at most once per
    /// second.
    [[nodiscard]] bool present() noexcept;
    /// The file's first 4 KiB as of the last probe ("" while absent).
    [[nodiscard]] std::string contents() const;
    /// Changes whenever a probe finds the file newly present or with
    /// different contents: a consumer with a budget (the draw dump's `max`)
    /// restarts it on a new generation.
    [[nodiscard]] u64 generation() const noexcept;

    [[nodiscard]] std::string path() const;
    /// Tests: look at @p path instead, and probe it on the next present().
    void set_path(std::string path);

private:
    void probe(u64 now) noexcept;

    mutable std::mutex m_mutex;
    std::string m_path;
    std::string m_contents;
    std::atomic<u64> m_next_probe{0};
    std::atomic<bool> m_present{false};
    std::atomic<u64> m_generation{0};
};

/// /tmp/vitreo-d3d11-caps: the device capability log.
[[nodiscard]] MarkerFile& caps_marker() noexcept;
/// /tmp/vitreo-draw-dump: the draw dump; its contents select the draws.
[[nodiscard]] MarkerFile& draw_dump_marker() noexcept;

/// Where lines go: stderr unless a sink is installed (tests). The sink gets
/// the whole line without a newline.
using LogSink = void (*)(void* user, std::string_view line);
void set_log_sink(LogSink sink, void* user) noexcept;
/// Print `[<tag>] pid=<pid> <text>`.
void emit(std::string_view tag, std::string_view text);

[[nodiscard]] inline bool caps_log_enabled() noexcept {
    return caps_marker().present();
}

/// printf into a std::string (the formats below are built from this).
[[nodiscard]] std::string strprintf(const char* format, ...)
    __attribute__((format(printf, 1, 2)));

/// Bytes as hex, grouped per 4-byte word: "01000000 ff000000 0a00".
[[nodiscard]] std::string hex_words(std::span<const u8> bytes);

namespace caps {

inline constexpr std::string_view kTag = "d3d11-caps";

/// D3D11_FEATURE_* name for @p feature (d3d11.h, Windows Kits 10.0.26100.0),
/// "?" past the last.
[[nodiscard]] std::string_view feature_name(u32 feature) noexcept;

/// `D3D11CreateDevice driver_type=1 flags=0x0 requested_fl=0xb100,0xb000
/// chosen_fl=0xb000 hr=0x00000000`; requested_fl=default when the caller
/// passed no levels, chosen_fl=none when none was chosen.
[[nodiscard]] std::string format_create_device(
    std::string_view api, u32 driver_type, u32 flags,
    std::span<const u32> requested_levels, bool levels_given, u32 chosen_level,
    u32 hr);

/// `CheckFeatureSupport feature=5 name=D3D11_OPTIONS size=56
/// hr=0x00000000 data=<hex words>`; FORMAT_SUPPORT / FORMAT_SUPPORT2 with
/// their 8-byte struct add `format=<DXGI_FORMAT> support=0x<mask>`.
/// @p data is empty (and `data=null` printed) for a NULL pointer.
[[nodiscard]] std::string format_check_feature_support(
    u32 feature, u32 size, u32 hr, bool data_given,
    std::span<const u8> data);

/// `CheckFormatSupport format=28 hr=0x00000000 support=0x000fffff`.
[[nodiscard]] std::string format_check_format_support(u32 format, u32 hr,
                                                      u32 mask);

/// `CheckMultisampleQualityLevels format=28 samples=4 hr=0x00000000
/// levels=1`; the Levels1 form adds flags=0x<flags>.
[[nodiscard]] std::string format_check_multisample(std::string_view api,
                                                   u32 format, u32 samples,
                                                   bool has_flags, u32 flags,
                                                   u32 hr, u32 levels);

/// What an IDXGIAdapter::GetDesc / GetDesc1 / GetDesc2 call returned.
struct AdapterDesc {
    std::string_view api;
    std::string description;
    u32 vendor_id{0};
    u32 device_id{0};
    u32 subsystem_id{0};
    u32 revision{0};
    u64 dedicated_video_memory{0};
    u64 dedicated_system_memory{0};
    u64 shared_system_memory{0};
    u32 luid_low{0};
    i32 luid_high{0};
    /// GetDesc1 / GetDesc2: DXGI_ADAPTER_FLAG.
    bool has_flags{false};
    u32 flags{0};
    /// GetDesc2: the preemption granularities.
    bool has_preemption{false};
    u32 graphics_preemption{0};
    u32 compute_preemption{0};
};
/// `Adapter IDXGIAdapter1::GetDesc1 description="..." vendor=0x106b ...`.
[[nodiscard]] std::string format_adapter_desc(const AdapterDesc& desc);

/// Log one line of @p text under the caps tag when the marker is present.
void log(std::string_view text);

}  // namespace caps

}  // namespace vitreo::winapi::live_diag
