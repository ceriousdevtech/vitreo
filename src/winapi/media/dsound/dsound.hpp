#pragma once
/// @file dsound.hpp
/// @brief DirectSound frontend: IDirectSound(8) / IDirectSoundBuffer(8)
/// playback, IDirectSoundCapture(8) / IDirectSoundCaptureBuffer(8) capture,
/// IDirectSoundNotify on both, IDirectSoundFullDuplex, and the in-proc class
/// factory for the capture classes.
///
/// Sources: Microsoft Learn (DirectSound reference), Windows Kits
/// 10.0.26100.0 dsound.h (dsound_capture_contract.inc, generated), the real
/// dsound.dll export table, and the Windows runs
/// tools/oracle_probe/audio_capture_contract_probe{32,64}.expected.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <array>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vitreo { class GuestMemory; }
namespace vitreo::audio {
class IAudioRenderer;
class IAudioVoice;
class IAudioCapture;
class IAudioCaptureStream;
}

namespace vitreo::winapi {

class Marshaler;

namespace dsound {
#include "winapi/media/dsound/dsound_capture_contract.inc"

// Cooperative levels.
constexpr u32 DSSCL_NORMAL = 1;
constexpr u32 DSSCL_PRIORITY = 2;
constexpr u32 DSSCL_EXCLUSIVE = 3;
constexpr u32 DSSCL_WRITEPRIMARY = 4;

// DSBUFFERDESC / DSBCAPS flags.
constexpr u32 DSBCAPS_PRIMARYBUFFER = 0x00000001u;
constexpr u32 DSBCAPS_STATIC = 0x00000002u;
constexpr u32 DSBCAPS_LOCHARDWARE = 0x00000004u;
constexpr u32 DSBCAPS_LOCSOFTWARE = 0x00000008u;
constexpr u32 DSBCAPS_CTRL3D = 0x00000010u;
constexpr u32 DSBCAPS_CTRLFREQUENCY = 0x00000020u;
constexpr u32 DSBCAPS_CTRLPAN = 0x00000040u;
constexpr u32 DSBCAPS_CTRLVOLUME = 0x00000080u;
constexpr u32 DSBCAPS_STICKYFOCUS = 0x00004000u;
constexpr u32 DSBCAPS_GLOBALFOCUS = 0x00008000u;
constexpr u32 DSBCAPS_GETCURRENTPOSITION2 = 0x00010000u;
constexpr u32 DSBCAPS_LOCDEFER = 0x00040000u;

constexpr u32 DSBPLAY_LOOPING = 0x00000001u;
constexpr u32 DSBSTATUS_PLAYING = 0x00000001u;
constexpr u32 DSBSTATUS_LOOPING = 0x00000004u;

constexpr u32 DSBLOCK_FROMWRITECURSOR = 0x00000001u;
constexpr u32 DSBLOCK_ENTIREBUFFER = 0x00000002u;

constexpr i32 DSBVOLUME_MIN = -10000;
constexpr i32 DSBVOLUME_MAX = 0;
constexpr i32 DSBPAN_LEFT = -10000;
constexpr i32 DSBPAN_RIGHT = 10000;
} // namespace dsound

/// Owns DirectSound COM state and maps it onto the frontend-neutral audio
/// renderer and capture contracts. Contains no platform audio types.
///
/// Every COM object lives in a guest region as `{vtable*}` blocks whose slots
/// are pointer-width for the guest (4 bytes on x86, 8 on x64); every pointer
/// out-parameter is written pointer-width.
///
/// Threading: guest threads call the methods; capture data arrives on a host
/// audio thread (capture_pump) and render position notifications come from
/// a manager-owned host thread. Both only copy samples and SetEvent. All
/// state is guarded by m_mutex; host streams are started, stopped and
/// destroyed with m_mutex released, because a stream may wait for its data
/// callback, which takes m_mutex.
class DirectSoundManager {
public:
    explicit DirectSoundManager(audio::IAudioRenderer* renderer) noexcept
        : m_renderer(renderer) {}
    ~DirectSoundManager();

    void set_memory(GuestMemory* memory) noexcept { m_memory = memory; }
    void set_thunk_registry(ThunkRegistry* registry) noexcept { m_thunks = registry; }
    void set_audio_capture(audio::IAudioCapture* capture) noexcept { m_capture = capture; }
    /// The guest's pointer width. Fixed for the process; recorded before the
    /// first object is created (the create thunks pass the caller's mode).
    void note_guest_bitness(bool x64) noexcept;
    /// SetEvent for notification and capture events (DSBPOSITIONNOTIFY).
    using EventSignaler = std::function<bool(u64 handle)>;
    void set_event_signaler(EventSignaler signaler) { m_signal = std::move(signaler); }

    using GuestCall = std::function<u64(GuestAddr, const u64*, usize, bool)>;
    void set_guest_call(GuestCall call) { m_guest_call = std::move(call); }
    [[nodiscard]] u32 enumerate(GuestAddr callback, GuestAddr context, bool wide,
                                bool capture, bool x64);
    [[nodiscard]] u32 get_device_id(GuestAddr source, GuestAddr destination);
    [[nodiscard]] u32 direct_sound_create(GuestAddr guid, GuestAddr output,
                                          GuestAddr outer, bool version8 = false);
    [[nodiscard]] u32 query_interface(GuestAddr object, GuestAddr iid,
                                      GuestAddr output);
    [[nodiscard]] u32 add_ref(GuestAddr object);
    [[nodiscard]] u32 release(GuestAddr object);
    [[nodiscard]] u32 set_cooperative_level(GuestAddr object, u64 hwnd, u32 level);
    [[nodiscard]] u32 get_caps(GuestAddr object, GuestAddr caps);
    [[nodiscard]] u32 create_sound_buffer(GuestAddr object, GuestAddr descriptor,
                                          GuestAddr output, GuestAddr outer);
    [[nodiscard]] u32 duplicate_sound_buffer(GuestAddr object, GuestAddr source,
                                             GuestAddr output);

    [[nodiscard]] u32 buffer_query_interface(GuestAddr buffer, GuestAddr iid,
                                             GuestAddr output);
    [[nodiscard]] u32 buffer_add_ref(GuestAddr buffer);
    [[nodiscard]] u32 buffer_release(GuestAddr buffer);
    [[nodiscard]] u32 buffer_get_caps(GuestAddr buffer, GuestAddr caps);
    [[nodiscard]] u32 buffer_get_current_position(GuestAddr buffer, GuestAddr play,
                                                  GuestAddr write);
    [[nodiscard]] u32 buffer_get_format(GuestAddr buffer, GuestAddr format,
                                        u32 size, GuestAddr written);
    [[nodiscard]] u32 buffer_get_volume(GuestAddr buffer, GuestAddr volume);
    [[nodiscard]] u32 buffer_get_pan(GuestAddr buffer, GuestAddr pan);
    [[nodiscard]] u32 buffer_get_frequency(GuestAddr buffer, GuestAddr frequency);
    [[nodiscard]] u32 buffer_get_status(GuestAddr buffer, GuestAddr status);
    [[nodiscard]] u32 buffer_lock(GuestAddr buffer, u32 offset, u32 bytes,
                                  GuestAddr ptr1, GuestAddr bytes1,
                                  GuestAddr ptr2, GuestAddr bytes2, u32 flags);
    [[nodiscard]] u32 buffer_unlock(GuestAddr buffer, GuestAddr ptr1, u32 bytes1,
                                    GuestAddr ptr2, u32 bytes2);
    [[nodiscard]] u32 buffer_play(GuestAddr buffer, u32 reserved, u32 priority,
                                  u32 flags);
    [[nodiscard]] u32 buffer_stop(GuestAddr buffer);
    [[nodiscard]] u32 buffer_set_current_position(GuestAddr buffer, u32 position);
    [[nodiscard]] u32 buffer_set_format(GuestAddr buffer, GuestAddr format);
    [[nodiscard]] u32 buffer_set_volume(GuestAddr buffer, i32 volume);
    [[nodiscard]] u32 buffer_set_pan(GuestAddr buffer, i32 pan);
    [[nodiscard]] u32 buffer_set_frequency(GuestAddr buffer, u32 frequency);
    [[nodiscard]] u32 buffer_restore(GuestAddr buffer);
    /// IDirectSoundBuffer8 (secondary buffers of an IDirectSound8 device).
    [[nodiscard]] u32 buffer_set_fx(GuestAddr buffer, u32 count, GuestAddr descs,
                                    GuestAddr results);
    [[nodiscard]] u32 buffer_acquire_resources(GuestAddr buffer, u32 flags, u32 count,
                                               GuestAddr results);
    [[nodiscard]] u32 buffer_get_object_in_path(GuestAddr buffer, GuestAddr object_guid,
                                                u32 index, GuestAddr iid,
                                                GuestAddr output);

    /// IDirectSound3DListener (QI'd from a CTRL3D primary buffer).
    [[nodiscard]] u32 listener_query_interface(GuestAddr listener, GuestAddr iid,
                                               GuestAddr output);
    [[nodiscard]] u32 listener_add_ref(GuestAddr listener);
    [[nodiscard]] u32 listener_release(GuestAddr listener);
    [[nodiscard]] u32 listener_get_all_parameters(GuestAddr listener,
                                                 GuestAddr params);
    [[nodiscard]] u32 listener_set_all_parameters(GuestAddr listener,
                                                 GuestAddr params, u32 apply);
    [[nodiscard]] u32 listener_get_distance_factor(GuestAddr listener,
                                                   GuestAddr out);
    [[nodiscard]] u32 listener_set_distance_factor(GuestAddr listener,
                                                   u32 factor_bits, u32 apply);
    [[nodiscard]] u32 listener_get_doppler_factor(GuestAddr listener,
                                                  GuestAddr out);
    [[nodiscard]] u32 listener_set_doppler_factor(GuestAddr listener,
                                                  u32 factor_bits, u32 apply);
    [[nodiscard]] u32 listener_get_rolloff_factor(GuestAddr listener,
                                                  GuestAddr out);
    [[nodiscard]] u32 listener_set_rolloff_factor(GuestAddr listener,
                                                  u32 factor_bits, u32 apply);
    [[nodiscard]] u32 listener_get_orientation(GuestAddr listener,
                                               GuestAddr front, GuestAddr top);
    [[nodiscard]] u32 listener_set_orientation(GuestAddr listener,
                                               u32 fx, u32 fy, u32 fz,
                                               u32 tx, u32 ty, u32 tz,
                                               u32 apply);
    [[nodiscard]] u32 listener_get_position(GuestAddr listener, GuestAddr out);
    [[nodiscard]] u32 listener_set_position(GuestAddr listener, u32 x, u32 y,
                                            u32 z, u32 apply);
    [[nodiscard]] u32 listener_get_velocity(GuestAddr listener, GuestAddr out);
    [[nodiscard]] u32 listener_set_velocity(GuestAddr listener, u32 x, u32 y,
                                            u32 z, u32 apply);
    [[nodiscard]] u32 listener_commit_deferred(GuestAddr listener);

    [[nodiscard]] u32 cooperative_level(GuestAddr object) const;

    // ---- Capture -----------------------------------------------------------
    /// DirectSoundCaptureCreate(8)(pcGuidDevice, ppDSC, pUnkOuter).
    [[nodiscard]] u32 capture_create(GuestAddr guid, GuestAddr output, GuestAddr outer,
                                     bool version8);
    /// The methods of IDirectSoundCapture8; `object` is any interface pointer
    /// of the capture object (its IUnknown facet included).
    [[nodiscard]] u32 capture_query_interface(GuestAddr object, GuestAddr iid,
                                              GuestAddr output);
    [[nodiscard]] u32 capture_add_ref(GuestAddr object);
    [[nodiscard]] u32 capture_release(GuestAddr object);
    [[nodiscard]] u32 capture_create_buffer(GuestAddr object, GuestAddr descriptor,
                                            GuestAddr output, GuestAddr outer);
    [[nodiscard]] u32 capture_get_caps(GuestAddr object, GuestAddr caps);
    [[nodiscard]] u32 capture_initialize(GuestAddr object, GuestAddr guid);

    /// IDirectSoundCaptureBuffer8; `buffer` is any interface pointer of the
    /// buffer object (IDirectSoundNotify and IUnknown facets included).
    [[nodiscard]] u32 cbuffer_query_interface(GuestAddr buffer, GuestAddr iid,
                                              GuestAddr output);
    [[nodiscard]] u32 cbuffer_add_ref(GuestAddr buffer);
    [[nodiscard]] u32 cbuffer_release(GuestAddr buffer);
    [[nodiscard]] u32 cbuffer_get_caps(GuestAddr buffer, GuestAddr caps);
    [[nodiscard]] u32 cbuffer_get_current_position(GuestAddr buffer, GuestAddr capture,
                                                   GuestAddr read);
    [[nodiscard]] u32 cbuffer_get_format(GuestAddr buffer, GuestAddr format, u32 size,
                                         GuestAddr written);
    [[nodiscard]] u32 cbuffer_get_status(GuestAddr buffer, GuestAddr status);
    [[nodiscard]] u32 cbuffer_initialize(GuestAddr buffer, GuestAddr capture,
                                         GuestAddr descriptor);
    [[nodiscard]] u32 cbuffer_lock(GuestAddr buffer, u32 offset, u32 bytes,
                                   GuestAddr ptr1, GuestAddr bytes1, GuestAddr ptr2,
                                   GuestAddr bytes2, u32 flags);
    [[nodiscard]] u32 cbuffer_start(GuestAddr buffer, u32 flags);
    [[nodiscard]] u32 cbuffer_stop(GuestAddr buffer);
    [[nodiscard]] u32 cbuffer_unlock(GuestAddr buffer, GuestAddr ptr1, u32 bytes1,
                                     GuestAddr ptr2, u32 bytes2);
    [[nodiscard]] u32 cbuffer_get_object_in_path(GuestAddr buffer, GuestAddr object_guid,
                                                 u32 index, GuestAddr iid,
                                                 GuestAddr output);
    [[nodiscard]] u32 cbuffer_get_fx_status(GuestAddr buffer, u32 count, GuestAddr status);

    /// IDirectSoundNotify on a capture buffer or a CTRLPOSITIONNOTIFY render
    /// buffer.
    [[nodiscard]] u32 notify_query_interface(GuestAddr notify, GuestAddr iid,
                                             GuestAddr output);
    [[nodiscard]] u32 notify_add_ref(GuestAddr notify);
    [[nodiscard]] u32 notify_release(GuestAddr notify);
    [[nodiscard]] u32 notify_set_positions(GuestAddr notify, u32 count,
                                           GuestAddr positions);

    /// DirectSoundFullDuplexCreate and IDirectSoundFullDuplex.
    [[nodiscard]] u32 full_duplex_create(GuestAddr capture_guid, GuestAddr render_guid,
                                         GuestAddr capture_desc, GuestAddr render_desc,
                                         u64 hwnd, u32 level, GuestAddr output,
                                         GuestAddr capture_buffer, GuestAddr render_buffer,
                                         GuestAddr outer);
    [[nodiscard]] u32 full_duplex_query_interface(GuestAddr object, GuestAddr iid,
                                                  GuestAddr output);
    [[nodiscard]] u32 full_duplex_add_ref(GuestAddr object);
    [[nodiscard]] u32 full_duplex_release(GuestAddr object);
    [[nodiscard]] u32 full_duplex_initialize(GuestAddr object, GuestAddr capture_guid,
                                             GuestAddr render_guid, GuestAddr capture_desc,
                                             GuestAddr render_desc, u64 hwnd, u32 level,
                                             GuestAddr capture_buffer,
                                             GuestAddr render_buffer);

    /// DllGetClassObject / IClassFactory for CLSID_DirectSoundCapture(8) and
    /// CLSID_DirectSoundFullDuplex.
    [[nodiscard]] u32 dll_get_class_object(GuestAddr clsid, GuestAddr iid, GuestAddr output);
    [[nodiscard]] u32 factory_query_interface(GuestAddr factory, GuestAddr iid,
                                              GuestAddr output);
    [[nodiscard]] u32 factory_add_ref(GuestAddr factory);
    [[nodiscard]] u32 factory_release(GuestAddr factory);
    [[nodiscard]] u32 factory_create_instance(GuestAddr factory, GuestAddr outer,
                                              GuestAddr iid, GuestAddr output);
    [[nodiscard]] u32 factory_lock_server(GuestAddr factory, u32 lock);

private:
    struct WaveFormat {
        u16 format_tag{0};
        u16 channels{0};
        u32 samples_per_second{0};
        u32 average_bytes_per_second{0};
        u16 block_align{0};
        u16 bits_per_sample{0};
    };

    struct Object {
        u32 ref_count{1};
        u64 hwnd{0};
        u32 cooperative_level{0};
        bool version8{false};
    };

    struct NotifyPosition {
        u32 offset{0};
        u64 event{0};
    };

    struct BufferObject {
        u32 ref_count{1};
        u32 flags{0};
        u32 bytes{0};
        GuestAddr samples{0};   ///< guest-visible PCM ring
        u32 mapped_bytes{0};
        bool primary{false};
        bool locked{false};
        bool looping{false};
        bool version8{false};
        /// Duplicates share the original's sample ring and must not free it.
        bool owns_samples{false};
        u32 lock_offset{0};
        u32 lock_bytes{0};
        i32 volume{0};
        i32 pan{0};
        u32 frequency{0};
        WaveFormat format{};
        GuestAddr listener{0};  ///< IDirectSound3DListener COM object, if any
        GuestAddr notify{0};    ///< IDirectSoundNotify facet, if any
        std::vector<NotifyPosition> notifications;
        /// Play cursor at the last notification poll, and whether the voice
        /// was playing then (a stop the poller sees fires DSBPN_OFFSETSTOP).
        u32 last_cursor{0};
        bool was_playing{false};
        std::unique_ptr<audio::IAudioVoice> voice;
    };

    struct ListenerObject {
        u32 ref_count{1};
        GuestAddr buffer{0};
        float distance_factor{1.0f};
        float doppler_factor{1.0f};
        float rolloff_factor{1.0f};
        float position[3]{0, 0, 0};
        float velocity[3]{0, 0, 0};
        float orient_front[3]{0, 0, 1};
        float orient_top[3]{0, 1, 0};
    };

    struct CaptureObject {
        u32 refs{1};
        bool initialized{false};
        /// Created through DirectSoundCaptureCreate8 / CLSID_DirectSoundCapture8:
        /// its buffers expose IDirectSoundCaptureBuffer8 and effects.
        bool version8{false};
        std::string device_uid;
        GuestAddr unknown{0};
        GuestAddr buffer{0};
    };

    struct CaptureBufferObject {
        u32 refs{1};
        GuestAddr owner{0};
        bool version8{false};
        u32 flags{0};
        u32 bytes{0};
        GuestAddr samples{0};
        u32 mapped_bytes{0};
        /// The WAVEFORMATEX(TENSIBLE) exactly as GetFormat returns it.
        std::vector<u8> format;
        u16 block_align{0};
        u32 fx_count{0};
        GuestAddr notify{0};
        GuestAddr unknown{0};
        std::vector<NotifyPosition> notifications;
        bool capturing{false};
        bool looping{false};
        u32 read_pos{0};
        bool locked{false};
        GuestAddr lock_ptr1{0};
        /// Measured: a Lock that would wrap but was given no second pointer
        /// stops at the end of the buffer, and after its Unlock no further
        /// Lock or Unlock on that buffer succeeds (Windows 11).
        bool lock_truncated{false};
        bool wedged{false};
        std::shared_ptr<audio::IAudioCaptureStream> stream;
        std::vector<u8> scratch;
    };

    struct FullDuplexObject {
        u32 refs{1};
        bool initialized{false};
        GuestAddr capture{0};
        GuestAddr render{0};
    };

    enum class FactoryClass : u8 { Capture, Capture8, FullDuplex };
    struct FactoryObject {
        u32 refs{1};
        FactoryClass cls{FactoryClass::Capture};
    };

    enum class Vtable : u8 {
        DSound, Buffer, Listener, Capture, CaptureBuffer, Notify, CaptureUnknown,
        CaptureBufferUnknown, FullDuplex, Factory, Count
    };

    [[nodiscard]] bool ensure_storage();
    [[nodiscard]] GuestAddr allocate_object(u32 bytes);
    void free_object(GuestAddr object, u32 bytes);
    [[nodiscard]] GuestAddr new_object(Vtable vtable, u32 interfaces = 1);
    [[nodiscard]] u32 pointer_size() const noexcept { return m_x64 ? 8u : 4u; }
    [[nodiscard]] bool write_ptr(GuestAddr address, u64 value) const;
    [[nodiscard]] bool read_guid(GuestAddr address, std::array<u8, 16>& out) const;
    [[nodiscard]] Object* find_object(GuestAddr object);
    [[nodiscard]] const Object* find_object(GuestAddr object) const;
    [[nodiscard]] BufferObject* find_buffer(GuestAddr buffer);
    [[nodiscard]] ListenerObject* find_listener(GuestAddr listener);
    [[nodiscard]] CaptureObject* find_capture(GuestAddr object, GuestAddr* primary = nullptr);
    [[nodiscard]] CaptureBufferObject* find_cbuffer(GuestAddr buffer,
                                                    GuestAddr* primary = nullptr);
    [[nodiscard]] bool read_wave_format(GuestAddr address, WaveFormat& out) const;
    void write_wave_format(GuestAddr address, const WaveFormat& format) const;
    [[nodiscard]] bool create_voice(BufferObject& state);
    [[nodiscard]] u32 ensure_listener(BufferObject& buffer, GuestAddr buffer_addr,
                                      GuestAddr output);
    [[nodiscard]] u32 buffer_release_locked(GuestAddr buffer,
                                            std::vector<std::unique_ptr<audio::IAudioVoice>>& dead);
    /// Resolve a capture device GUID (NULL, GUID_NULL, DSDEVID_* or a device
    /// GUID) to a host device uid. False: no such device (DSERR_NODRIVER).
    [[nodiscard]] bool resolve_capture_device(GuestAddr guid, std::string& uid) const;
    [[nodiscard]] u32 create_capture_object(bool initialized, bool version8,
                                            const std::string& uid, GuestAddr& object);
    [[nodiscard]] u32 create_capture_buffer_locked(GuestAddr capture_ptr,
                                                   GuestAddr descriptor,
                                                   GuestAddr& buffer,
                                                   std::shared_ptr<audio::IAudioCaptureStream>& opened);
    [[nodiscard]] u32 set_positions(std::vector<NotifyPosition>& list, u32 buffer_bytes,
                                    u32 count, GuestAddr positions);
    void destroy_cbuffer_locked(GuestAddr buffer,
                                std::vector<std::shared_ptr<audio::IAudioCaptureStream>>& dead);
    void destroy_capture_locked(GuestAddr object,
                                std::vector<std::shared_ptr<audio::IAudioCaptureStream>>& dead);
    /// Host audio thread: move captured frames into the guest buffer and fire
    /// the notifications they cross.
    void capture_pump(GuestAddr buffer);
    void signal(u64 handle) const;
    void signal_crossed(const std::vector<NotifyPosition>& list, u32 from, u32 to) const;
    /// Render position notifications: start the poller when a playing
    /// buffer has notifications.
    void wake_notify_thread_locked();
    void notify_thread_loop();
    [[nodiscard]] u32 full_duplex_build(GuestAddr capture_guid, GuestAddr render_guid,
                                        GuestAddr capture_desc, GuestAddr render_desc,
                                        u64 hwnd, u32 level, GuestAddr& capture,
                                        GuestAddr& render, GuestAddr capture_buffer,
                                        GuestAddr render_buffer);

    GuestCall m_guest_call;
    EventSignaler m_signal;
    audio::IAudioRenderer* m_renderer{nullptr};
    audio::IAudioCapture* m_capture{nullptr};
    GuestMemory* m_memory{nullptr};
    ThunkRegistry* m_thunks{nullptr};
    bool m_x64{false};
    GuestAddr m_region_base{0};
    usize m_region_size{0};
    std::array<GuestAddr, static_cast<usize>(Vtable::Count)> m_vtables{};
    GuestAddr m_next_object{0};
    std::unordered_map<u32, std::vector<GuestAddr>> m_free_objects;
    std::unordered_map<GuestAddr, Object> m_objects;
    std::unordered_map<GuestAddr, BufferObject> m_buffers;
    std::unordered_map<GuestAddr, ListenerObject> m_listeners;
    std::unordered_map<GuestAddr, CaptureObject> m_captures;
    std::unordered_map<GuestAddr, CaptureBufferObject> m_cbuffers;
    std::unordered_map<GuestAddr, FullDuplexObject> m_full_duplex;
    std::unordered_map<GuestAddr, FactoryObject> m_factories;
    /// Secondary interface pointer (IDirectSoundNotify, IUnknown facet) ->
    /// the object's primary interface pointer.
    std::unordered_map<GuestAddr, GuestAddr> m_facets;
    mutable std::recursive_mutex m_mutex;
    std::thread m_notify_thread;
    std::condition_variable_any m_notify_wake;
    bool m_notify_stop{false};
};

[[nodiscard]] DllExports create_dsound_thunks(DirectSoundManager& manager);

} // namespace vitreo::winapi
