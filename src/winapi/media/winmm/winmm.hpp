#pragma once

#include "winapi/core/thunk_registry.hpp"
#include "winapi/media/winmm/timer_scheduler.hpp"
#include <vitreo/common.hpp>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vitreo { class GuestMemory; }
namespace vitreo::capsule { class Vfs; }
namespace vitreo::windowing {
class WaveOutput;
}
namespace vitreo::audio {
class IAudioCapture;
class IAudioCaptureStream;
}

namespace vitreo::winapi {

/// Win32 WAVEHDR layout. mmeapi.h declares it under pshpack1, so the x64
/// form keeps its pointer-width fields unaligned (s7_winmm_probe `sizes`:
/// x86 32 bytes, x64 48). A stream records the bitness of the guest that
/// opened it; these offsets must never use host pointer sizes.
struct WaveHdrLayout {
    u32 data_offset;          ///< LPSTR lpData
    u32 buffer_length_offset; ///< DWORD dwBufferLength
    u32 bytes_recorded_offset;///< DWORD dwBytesRecorded
    u32 user_offset;          ///< DWORD_PTR dwUser
    u32 flags_offset;         ///< DWORD dwFlags
    u32 loops_offset;         ///< DWORD dwLoops
    u32 size;
};
inline constexpr WaveHdrLayout wavehdr32{0, 4, 8, 12, 16, 20, 32};
inline constexpr WaveHdrLayout wavehdr64{0, 8, 12, 16, 24, 28, 48};
[[nodiscard]] constexpr const WaveHdrLayout& wavehdr_layout(bool is_64bit) noexcept {
    return is_64bit ? wavehdr64 : wavehdr32;
}

/// Milliseconds since host startup, truncated to the Win32 DWORD range.
[[nodiscard]] u32 time_get_time() noexcept;

/// Manager for winmm.dll multimedia APIs (PlaySoundW, waveOut*, etc.)
class WinmmManager {
public:
    WinmmManager(GuestMemory& memory, windowing::WaveOutput& wave_output,
                 capsule::Vfs& vfs)
        : m_memory{memory}, m_wave_output{wave_output}, m_vfs{vfs} {}

    ~WinmmManager();

    /// Set the PE resource info (called after PE is loaded)
    void set_resource_info(GuestAddr image_base, u32 rsrc_rva, u32 rsrc_size);

    /// Access guest memory for thunks
    GuestMemory& memory() noexcept { return m_memory; }

    // Thunk implementations
    void impl_PlaySoundW(Marshaler& m);
    void impl_PlaySoundA(Marshaler& m);
    void impl_sndPlaySoundA(Marshaler& m);
    void impl_sndPlaySoundW(Marshaler& m);
    void impl_waveOutGetNumDevs(Marshaler& m);
    void impl_waveOutGetDevCapsA(Marshaler& m);
    void impl_waveOutGetDevCapsW(Marshaler& m);
    void impl_waveOutOpen(Marshaler& m);
    void impl_waveOutClose(Marshaler& m);
    void impl_waveOutPrepareHeader(Marshaler& m);
    void impl_waveOutUnprepareHeader(Marshaler& m);
    void impl_waveOutWrite(Marshaler& m);
    void impl_waveOutReset(Marshaler& m);
    void impl_waveOutPause(Marshaler& m);
    void impl_waveOutRestart(Marshaler& m);
    void impl_waveOutGetPosition(Marshaler& m);
    void impl_waveOutGetID(Marshaler& m);
    void impl_waveOutMessage(Marshaler& m);
    void impl_waveOutSetVolume(Marshaler& m);
    void impl_waveOutGetVolume(Marshaler& m);
    void impl_waveOutBreakLoop(Marshaler& m);
    /// waveOutGet/SetPitch and waveOutGet/SetPlaybackRate.
    void impl_waveOutUnsupportedControl(Marshaler& m);
    // ---- waveIn (capture) ----
    /// The host capture backend; null (or no input device) means no waveIn
    /// device, which is what waveInGetNumDevs then reports.
    void set_audio_capture(audio::IAudioCapture* capture) noexcept { m_capture = capture; }
    void impl_waveInGetNumDevs(Marshaler& m);
    void impl_waveInGetDevCaps(Marshaler& m, bool wide);
    void impl_waveInOpen(Marshaler& m);
    void impl_waveInClose(Marshaler& m);
    void impl_waveInPrepareHeader(Marshaler& m);
    void impl_waveInUnprepareHeader(Marshaler& m);
    void impl_waveInAddBuffer(Marshaler& m);
    void impl_waveInStart(Marshaler& m);
    void impl_waveInStop(Marshaler& m);
    void impl_waveInReset(Marshaler& m);
    void impl_waveInGetPosition(Marshaler& m);
    void impl_waveInGetID(Marshaler& m);
    void impl_waveInMessage(Marshaler& m);
    void impl_mixerOpen(Marshaler& m);
    /// MMRESULT a mixer API reports for the object named by a HMIXEROBJ /
    /// UINT id plus its MIXER_OBJECTF_* flags. Vitreo has no mixer device.
    [[nodiscard]] u32 mixer_object_status(u64 object, u32 flags);
    void impl_mmioOpenA(Marshaler& m);
    void impl_mmioClose(Marshaler& m);
    void impl_mmioDescend(Marshaler& m);
    void impl_mmioRead(Marshaler& m);
    void impl_mmioAscend(Marshaler& m);

    /// True while at least one waveform output stream is open.
    [[nodiscard]] bool audio_open() const noexcept;

    // ---- Multimedia timers ----
    /// Register a timer and return its non-zero ID (0 when none is available).
    [[nodiscard]] u32 timer_create(u32 delay, u32 resolution, GuestAddr callback,
                                   u64 user_data, u32 flags);
    /// Retire a timer ID. False when the ID was never issued or already killed.
    [[nodiscard]] bool timer_destroy(u32 timer_id);

    /// The scheduler that actually dispatches timers. The run loop wires the
    /// guest-callback invoker into this, since only it can build the guest
    /// thread context a TIMEPROC needs.
    [[nodiscard]] MultimediaTimerScheduler& timers() noexcept { return m_timers; }

    /// Run a waveOutProc(HWAVEOUT, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR).
    /// Handles and DWORD_PTR arguments keep all 64 bits on x64.
    using WaveCallbackInvoker =
        std::function<void(GuestAddr proc, u64 wave_handle, u32 message,
                           u64 instance, u64 param1, u64 param2)>;
    /// WOM_DONE runs on a Vitreo-owned guest service thread: CoreAudio
    /// callbacks only enqueue completion records and never execute guest code
    /// on the host's real-time audio thread.
    void set_wave_callback_invoker(WaveCallbackInvoker invoker);
    /// WOM_OPEN and WOM_CLOSE run synchronously on the thread that called
    /// waveOutOpen/waveOutClose, before the call returns (s7_winmm_probe
    /// fn_open_cbs / close_cbs: "same" thread, cb_at_return=1). This invoker
    /// runs guest code on the calling guest thread.
    void set_wave_sync_callback_invoker(WaveCallbackInvoker invoker);

    /// CALLBACK_WINDOW: PostMessage(hwnd, MM_WOM_*, hwo, header).
    using WaveWindowNotifier =
        std::function<bool(u64 hwnd, u32 message, u64 wave_handle,
                           u64 param1)>;
    void set_wave_window_notifier(WaveWindowNotifier notifier);
    /// CALLBACK_THREAD: PostThreadMessage(thread_id, MM_WOM_*, hwo, header).
    using WaveThreadNotifier =
        std::function<bool(u32 thread_id, u32 message, u64 wave_handle,
                           u64 param1)>;
    void set_wave_thread_notifier(WaveThreadNotifier notifier);
    /// CALLBACK_EVENT: SetEvent(handle) on every WOM_* state change.
    using WaveEventSignaler = std::function<bool(u64 event_handle)>;
    void set_wave_event_signaler(WaveEventSignaler signaler);
    /// IsWindow, for waveOutOpen/mixerOpen CALLBACK_WINDOW validation.
    using WindowValidator = std::function<bool(u64 hwnd)>;
    void set_window_validator(WindowValidator validator);
    /// Creates an additional host output stream. The injected WaveOutput
    /// backs the first stream; every concurrently open waveOut handle and the
    /// PlaySound voice needs its own stream (Windows allows many:
    /// s7_winmm_probe multi_open).
    using WaveOutputFactory =
        std::function<std::unique_ptr<windowing::WaveOutput>()>;
    void set_wave_output_factory(WaveOutputFactory factory);

private:
    struct WaveStream;
    struct WaveInStream;
    /// Who a wave notification goes to (a waveOut or a waveIn handle).
    struct WaveCallbackTarget {
        u64 handle{0};
        GuestAddr callback{0};
        u32 callback_type{0};
        u64 instance{0};
        bool is_64bit{false};
    };

    struct WaveCallbackEvent {
        u64 stream_handle{0};
        GuestAddr callback{0};
        u32 callback_type{0};
        u64 instance{0};
        u32 message{0};
        u64 param1{0};
        u64 param2{0};
        bool complete_header{false};
        bool is_64bit{false};
        u64 sequence{0};
    };

    /// One host output stream with its owner (a waveOut handle or PlaySound).
    struct OutputLease {
        windowing::WaveOutput* output{nullptr};
        std::unique_ptr<windowing::WaveOutput> owned;
    };
    [[nodiscard]] bool acquire_output(OutputLease& lease);
    void release_output(OutputLease& lease);

    [[nodiscard]] std::shared_ptr<WaveStream> find_stream(u64 handle);
    /// Deliver WOM_OPEN/WOM_CLOSE on the calling thread (see
    /// set_wave_sync_callback_invoker).
    void notify_sync(const WaveStream& stream, u32 message);
    void notify_sync(const WaveCallbackTarget& target, u32 message);
    /// Queue a notification for the service thread; returns its sequence.
    u64 enqueue_wave_event_locked(const WaveStream& stream, u32 message,
                                  u64 param1, bool complete_header);
    u64 enqueue_wave_event_locked(const WaveCallbackTarget& target, u32 message,
                                  u64 param1, bool complete_header);
    [[nodiscard]] std::shared_ptr<WaveInStream> find_wavein(u64 handle);
    /// Stop and drop every waveIn stream (destruction).
    void close_all_wavein();
    /// Host audio thread: fill the queued waveIn headers.
    void wavein_pump(u64 handle);
    /// Return the head header with what it recorded (WIM_DATA). Caller holds
    /// m_wave_mutex.
    void complete_wavein_head_locked(WaveInStream& stream);
    /// Block until every event up to `sequence` has been delivered, unless
    /// called from the service thread itself.
    void wait_wave_events(u64 sequence);
    void wave_callback_loop();
    void stop_wave_callback_thread();
    void complete_wave_block(u64 stream_handle, u64 token);
    void deliver_wave_event(const WaveCallbackEvent& event);
    [[nodiscard]] u32 wave_device_count() const noexcept;

    // PlaySound voice (one per process: a new sound stops the previous one).
    struct SoundVoice;
    [[nodiscard]] bool play_sound(u32 flags, std::vector<u8> wav);
    void stop_sound(bool set_last_error_on_stop);
    void play_sound_entry(Marshaler& m, GuestAddr sound, GuestAddr hmod,
                          u32 flags, bool wide);

    MultimediaTimerScheduler m_timers;

    GuestMemory& m_memory;
    /// PlaySound plays through WaveOutput streams, which report format
    /// failures, completion (SND_SYNC), looping and busy state (SND_NOSTOP).
    windowing::WaveOutput& m_wave_output;
    capsule::Vfs& m_vfs;
    GuestAddr m_image_base = 0;
    u32 m_rsrc_rva = 0;
    u32 m_rsrc_size = 0;

    std::mutex m_wave_mutex;
    std::condition_variable m_wave_wake;
    std::condition_variable m_wave_delivered;
    std::deque<WaveCallbackEvent> m_wave_events;
    std::thread m_wave_thread;
    WaveCallbackInvoker m_wave_invoker;
    WaveCallbackInvoker m_wave_sync_invoker;
    WaveWindowNotifier m_wave_window_notifier;
    WaveThreadNotifier m_wave_thread_notifier;
    WaveEventSignaler m_wave_event_signaler;
    WindowValidator m_window_validator;
    WaveOutputFactory m_wave_output_factory;
    bool m_wave_stopping = false;
    u64 m_wave_event_sequence = 0;
    u64 m_wave_delivered_sequence = 0;
    bool m_primary_output_in_use = false;
    u64 m_next_wave_handle = 0xAA01;
    /// waveOutSetVolume on a device id; new streams start at it.
    u32 m_device_volume = 0x0000FFFF;
    std::unordered_map<u64, std::shared_ptr<WaveStream>> m_wave_streams;
    std::unordered_map<u64, std::shared_ptr<WaveInStream>> m_wavein_streams;
    audio::IAudioCapture* m_capture{nullptr};

    std::mutex m_sound_mutex;
    std::shared_ptr<SoundVoice> m_sound;
    /// An async sound was started and not yet torn down. Stopping it is
    /// what sets the last error to 0 (s7_winmm_probe ps_stop_idle).
    bool m_sound_async_pending = false;
    bool m_sound_async_started = false;

    struct MmioFile {
        std::vector<u8> bytes;
        usize cursor{0};
        std::string path;
    };
    u32 m_next_mmio_handle{0xAD00};
    std::unordered_map<u32, MmioFile> m_mmio_files;

};

/// Create winmm.dll exports backed by the manager.
DllExports create_winmm_thunks(WinmmManager& mgr);

} // namespace vitreo::winapi
